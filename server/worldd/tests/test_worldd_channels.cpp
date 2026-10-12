// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_channels.cpp: actor channels in net/connection: chains split
//  into partial bunches, the Connection's channel sequences, open and close
//
//  Bit for bit against the original server's bunches in fixtures/replay_data.bin:
//    ch 3  pkt#22   PlayerController open (chSeq 954/955: 3545 bits of exports,
//                   1314 of content) and ClientSetHUD (956/957: 873 + 173, the
//                   real bits), pkt#23 (958, 959), pkt#72..#77 (960..965: 1009
//                   bits of exports and 25,833 of content in 4 parts)
//    ch 6  pkt#28..#45  GameState open: 3473 bits of exports, 123,841 of content
//                   in 17 parts (954..971)
//    ch 14 pkt#62/#63   two export bunches (7329, 961 bits) and 6804 of content
//    ch 21 pkt#100..#26649  opens and closes, the sequence going on across them
//    ch 3  pkt#149  an unreliable chain
//  Not in the capture (UE's rules, from UChannel::Close / ReceivedAcks, PackageMapClient::ReceivedAck):
//    a closed channel index stays taken until its close is acked; AckEvents per chain part;
//    can_send_chain.  The worldd tests of the replicator on top of it are test_worldd_repl_acks.cpp.
//  The captured headers are as decoded from the packets (hdr bits, LSB-first
//  hex); the payloads are made up unless noted, only their sizes count.
// ============================================================================
#include "fake_client.h"
#include "net/connection.h"
#include "protocol/wire/ue5_primitives.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace aoc::worldd;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace wire = aoc::protocol::wire;
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

static ConnectionConfig server_config() {
    ConnectionConfig c;
    c.outer = kOuter;
    c.custom_field = kCf;
    c.server_initial_seq = 14265;
    c.client_initial_seq = 15870;
    return c;
}

/// The client's end: a Connection with the sequences the other way round.
static ConnectionConfig client_config() {
    ConnectionConfig c = server_config();
    std::swap(c.server_initial_seq, c.client_initial_seq);
    return c;
}
// ~ C:Y:B :: CoA ~

// -- bits -------------------------------------------------------------------

static emit::BunchWriter random_bits(size_t bits, uint32_t seed) {
    emit::BunchWriter w(bits / 8 + 8);
    uint32_t x = seed * 2654435761u + 12345u;
    for (size_t i = 0; i < bits; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        w.write_bit(static_cast<int>(x & 1));
    }
    return w;
}

static std::vector<uint8_t> unhex(const char* hex) {
    std::vector<uint8_t> out;
    const std::string s(hex);
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
    return out;
}

/// The first `bits` bits of a hex string (LSB-first bytes), one element each.
static std::vector<uint8_t> hex_bits(const char* hex, size_t bits) {
    const auto b = unhex(hex);
    std::vector<uint8_t> out;
    for (size_t i = 0; i < bits; ++i) out.push_back((b[i / 8] >> (i % 8)) & 1);
    return out;
}

static emit::BunchWriter writer_of(const char* hex, size_t bits) {
    const auto b = unhex(hex);
    emit::BunchWriter w(b.size() + 8);
    w.write_bit_range(b.data(), 0, bits);
    return w;
}

static std::vector<uint8_t> bits_of(const emit::BunchWriter& w) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < w.bit_pos(); ++i) out.push_back((w.data()[i / 8] >> (i % 8)) & 1);
    return out;
}

static std::vector<uint8_t> bits_of(const InBunch& b) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < b.bits; ++i) out.push_back((b.data[i / 8] >> (i % 8)) & 1);
    return out;
}

static std::vector<uint8_t> concat(std::vector<uint8_t> a, const std::vector<uint8_t>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// -- what came out of a Connection -------------------------------------------

/// One bunch in a datagram of the server: its parsed header, the header's bits,
/// the payload's bits, and which datagram of the batch it was in.
struct Seen {
    size_t packet = 0;
    wire::ParsedBunch b;
    std::vector<uint8_t> header;
    std::vector<uint8_t> payload;
};
using Datagrams = std::vector<std::vector<uint8_t>>;

static std::vector<Seen> parse(const Datagrams& pkts) {
    std::vector<Seen> out;
    for (size_t i = 0; i < pkts.size(); ++i) {
        const auto& p = pkts[i];
        const auto dp = conn::read_data_packet(p.data(), p.size(), wire::Direction::ServerToClient);
        if (!dp) continue;
        for (const auto& b : conn::packet_bunches(p.data(), p.size(), *dp)) {
            Seen s;
            s.packet = i;
            s.b = b;
            size_t off = b.data_start_bit - b.header_bits;
            for (size_t k = 0; k < b.header_bits; ++k) s.header.push_back(static_cast<uint8_t>(::ue5::read_bits(p.data(), p.size(), off, 1)));
            off = b.data_start_bit;
            for (size_t k = 0; k < b.bunch_data_bits; ++k) s.payload.push_back(static_cast<uint8_t>(::ue5::read_bits(p.data(), p.size(), off, 1)));
            out.push_back(std::move(s));
        }
    }
    return out;
}

static std::vector<Seen> on_channel(const std::vector<Seen>& all, uint32_t channel) {
    std::vector<Seen> out;
    for (const auto& s : all)
        if (s.b.channel == channel) out.push_back(s);
    return out;
}
// CoA and C-Y-B

/// A bunch of the capture: chSeq, header bits (count, LSB-first hex) and payload bits.
struct Captured {
    unsigned chseq;
    size_t hdr_bits;
    const char* hdr_hex;
    size_t bits;
};

/// How many of seen[first..] differ from the capture in chSeq, header bits or payload size.
static size_t mismatches(const std::vector<Seen>& seen, size_t first, const Captured* cap, size_t n) {
    size_t bad = 0;
    for (size_t i = 0; i < n; ++i) {
        const bool ok = first + i < seen.size() && seen[first + i].b.ch_sequence == cap[i].chseq &&
                        seen[first + i].header == hex_bits(cap[i].hdr_hex, cap[i].hdr_bits) &&
                        seen[first + i].payload.size() == cap[i].bits;
        if (!ok) {
            std::printf("    part %zu differs from the capture (chSeq %u)\n", i, cap[i].chseq);
            ++bad;
        }
    }
    return bad;
}
/// Counts a failure but prints nothing when it holds (for a check done in a loop).
#define CHECK_QUIET(cond) do { if (!(cond)) { std::printf("  [FAIL] %s\n", #cond); g_fail++; } } while (0)
#define CHECK_CAPTURE(seen, first, table, msg) \
    CHECK(mismatches(seen, first, table, sizeof(table) / sizeof(table[0])) == 0, msg)

static Chain chain(uint32_t channel, size_t export_bits, size_t payload_bits, bool open = false, uint32_t seed = 1) {
    Chain c;
    c.channel = channel;
    c.open = open;
    if (export_bits) c.exports.push_back(random_bits(export_bits, seed));
    c.payload = random_bits(payload_bits, seed + 100);
    return c;
}

/// Server packets to the client Connection, the client's acks back: the loop a
/// real pair of endpoints runs.  `drop` is a packet of `pkts` that does not arrive.
static std::vector<InBunch> deliver(Connection& client, const Datagrams& pkts, Clock::time_point t, size_t drop = SIZE_MAX) {
    std::vector<InBunch> got;
    for (size_t i = 0; i < pkts.size(); ++i) {
        if (i == drop) continue;
        for (auto& b : client.receive(pkts[i], t)) got.push_back(std::move(b));
    }
    return got;
}

static void return_acks(Connection& server, Connection& client, Clock::time_point t) {
    for (const auto& p : client.flush(t)) server.receive(p, t);
}

// -- the capture ------------------------------------------------------------

// ch 3, pkt#22: the PlayerController open and the ClientSetHUD RPC, one packet.
static const Captured kPkt22[] = {
    {954, 51, "d3a0ba27737603", 3545},  // open, exports, partial initial
    {955, 49, "34e0eecc2c5200", 1314},  // partial final
    {956, 49, "3428efc99c3600", 873},   // exports, partial initial
    {957, 49, "3460efccdc0a00", 173},   // partial final
};
// The ClientSetHUD chain's bits as captured: the BP_AOCHUD_C export and the RPC.
static const char* kHudExport =
    "020000001e2214eb02c1e9ea00000000000000000ace7b7ed7e3afa2d800000000000000000a00000000000000000000000000000000"
    "360000005e8ec2daca5eaa925eaed2c8cecae8e65e84a0be829e8690aa8800000000001800000084a0be829e8690aa88be8600a224de5801";
static const char* kHudRpc = "de08d43020f02142b12e109cae0e0000000000000000";
// ch 3, pkt#23: two bunches in one packet, neither partial.
static const Captured kPkt23[] = {
    {958, 46, "3480ef99bd1b", 3550},
    {959, 46, "34c0ef990f02", 263},
};
// ch 3, pkt#72..#76: 1009 bits of exports and 25,833 of content: 3 x 7616 + 2985.
static const Captured kPkt72[] = {
    {960, 49, "3428f0c91c3f00", 1009},  // exports, partial initial
    {961, 49, "3460f0c80cdc01", 7616},
    {962, 49, "34a0f0c80cdc01", 7616},
    {963, 49, "34e0f0c80cdc01", 7616},
    {964, 49, "3420f1cc9cba00", 2985},  // partial final
};
static const Captured kPkt77[] = {{965, 46, "3440f199d313", 2537}};
// ch 6, pkt#28..#45: the GameState open.
static const Captured kGameState[] = {
    {954, 51, "93a1ba27736403", 3473},  // open, exports, partial initial
    {955, 49, "64e0eec80cdc01", 7616}, {956, 49, "6420efc80cdc01", 7616}, {957, 49, "6460efc80cdc01", 7616},
    {958, 49, "64a0efc80cdc01", 7616}, {959, 49, "64e0efc80cdc01", 7616}, {960, 49, "6420f0c80cdc01", 7616},
    {961, 49, "6460f0c80cdc01", 7616}, {962, 49, "64a0f0c80cdc01", 7616}, {963, 49, "64e0f0c80cdc01", 7616},
    {964, 49, "6420f1c80cdc01", 7616}, {965, 49, "6460f1c80cdc01", 7616}, {966, 49, "64a0f1c80cdc01", 7616},
    {967, 49, "64e0f1c80cdc01", 7616}, {968, 49, "6420f2c80cdc01", 7616}, {969, 49, "6460f2c80cdc01", 7616},
    {970, 49, "64a0f2c80cdc01", 7616},
    {971, 49, "64e0f2cc1c7c00", 1985},  // partial final
};
// ch 14, pkt#62/#63: an export section of two bunches.
static const Captured kCh14[] = {
    {954, 51, "93a3ba27732807", 7329},  // open, exports, partial initial
    {955, 49, "e4e8eec81c3c00", 961},   // exports, partial
    {956, 49, "e420efcc4ca901", 6804},  // partial final
};
// ch 21: opens and closes; chSeq goes on across them.
static const Captured kCh21Open[] = {  // pkt#100, #101
    {954, 51, "53a5ba2773a005", 5761},
    {955, 49, "54e1eecc5cd701", 7541},
};
static const Captured kCh21Close3[] = {{956, 53, "1d55c07b660000", 0}};        // pkt#8332, reason 3
static const Captured kCh21Whole[] = {{957, 48, "5305bd67b64b", 2422}};         // pkt#8535: an open, whole, no exports
static const Captured kCh21Close1a[] = {{958, 53, "0d55e07b660000", 0}};        // pkt#8539, reason 1
static const Captured kCh21Split[] = {                                        // pkt#15976, #15977: an open in parts, no exports
    {959, 51, "5385bf27337007", 7616},
    {960, 49, "5421f0ccac4e01", 5354},
};
static const Captured kCh21Close1b[] = {{961, 53, "0d55107c660000", 0}};        // pkt#15979
// ch 3, pkt#149: an unreliable chain, 553 bits of exports then 1372.
static const Captured kUnreliable[] = {
    {0, 30, "30685204", 553},
    {0, 30, "3020b90a", 1372},
};
// -- C-Y-B x CoA --

// -- tests ------------------------------------------------------------------

static void test_pc_channel() {
    std::printf("ch 3: pkt#22, #23, #72..#77\n");
    Connection c(server_config(), t0);
    CHECK(c.send_chain(chain(3, 3545, 1314, true)), "the PlayerController open is queued (3545 bits of exports, 1314 of content)");
    Chain hud;
    hud.channel = 3;
    hud.exports.push_back(writer_of(kHudExport, 873));
    hud.payload = writer_of(kHudRpc, 173);
    CHECK(c.send_chain(hud), "then ClientSetHUD (873 bits of export, the 173-bit RPC)");
    CHECK(c.channel_open(3), "channel 3 is open");
    auto pkts = c.flush(t0);
    CHECK(pkts.size() == 1, "all four parts share one packet, as in pkt#22");
    auto seen = parse(pkts);
    CHECK(seen.size() == 4, "four bunches");
    CHECK_CAPTURE(seen, 0, kPkt22, "chSeq 954..957: the header bits are those of pkt#22, bit for bit");
    CHECK(seen.size() == 4 && seen[0].b.is_control && seen[0].b.is_open && !seen[0].b.is_close &&
              seen[0].b.is_reliable && seen[0].b.has_package_map_exports && seen[0].b.is_partial &&
              seen[0].b.partial_initial && !seen[0].b.partial_final && !seen[0].b.partial_has_comp_export &&
              seen[0].b.channel_name == "EName[102]",
          "the first part: bControl, bOpen, exports, partial initial, no custom-exports bit, EName 102");
    CHECK(seen.size() == 4 && !seen[1].b.is_control && !seen[1].b.is_open && !seen[1].b.has_package_map_exports &&
              seen[1].b.is_partial && !seen[1].b.partial_initial && seen[1].b.partial_final,
          "the second: content, partial final, no bOpen");
    CHECK(seen.size() == 4 && seen[2].payload == hex_bits(kHudExport, 873) && seen[3].payload == hex_bits(kHudRpc, 173),
          "the ClientSetHUD parts carry the export and the RPC bit for bit");

    CHECK(c.send_chain(chain(3, 0, 3550)) && c.send_chain(chain(3, 0, 263)), "two whole bunches");
    pkts = c.flush(t0 + milliseconds(1));
    seen = parse(pkts);
    CHECK(pkts.size() == 1 && seen.size() == 2, "in one packet, as pkt#23");
    CHECK(seen.size() == 2 && !seen[0].b.is_partial && !seen[1].b.is_partial, "neither is partial");
    CHECK_CAPTURE(seen, 0, kPkt23, "chSeq 958, 959: the header bits are those of pkt#23");

    CHECK(c.send_chain(chain(3, 1009, 25833)), "1009 bits of exports and 25,833 of content");
    pkts = c.flush(t0 + milliseconds(2));
    seen = parse(pkts);
    size_t biggest = 0;
    for (const auto& p : pkts) biggest = std::max(biggest, p.size());
    CHECK(pkts.size() == 5 && seen.size() == 5 && biggest <= conn::kMaxPacketBytes,
          "five parts in five packets of at most 1028 bytes");
    CHECK_CAPTURE(seen, 0, kPkt72, "chSeq 960..964: the header bits are those of pkt#72..#76");
    CHECK(c.send_chain(chain(3, 0, 2537)), "one more bunch");
    seen = parse(c.flush(t0 + milliseconds(3)));
    CHECK_CAPTURE(seen, 0, kPkt77, "chSeq 965: the header bits are those of pkt#77");
    CHECK(c.unacked_reliable(3) == 12, "twelve bunches, 954..965, in flight on the channel");
}

static void test_gamestate_open() {
    std::printf("ch 6: pkt#28..#45, the GameState open\n");
    Connection c(server_config(), t0);
    Chain gs = chain(6, 3473, 123841, true);
    CHECK(c.send_chain(gs), "3473 bits of exports and 123,841 of content are queued");
    const auto pkts = c.flush(t0);
    const auto seen = parse(pkts);
    CHECK(seen.size() == 18 && pkts.size() == 18, "18 parts in 18 packets (a 7616-bit part fills a packet)");
    CHECK_CAPTURE(seen, 0, kGameState, "chSeq 954..971: the header bits are those of pkt#28..#45, bit for bit");
    bool cut = true;
    const auto content = bits_of(gs.payload);
    std::vector<uint8_t> joined;
    for (size_t i = 1; i < seen.size(); ++i) joined = concat(joined, seen[i].payload);
    cut = joined == content;
    CHECK(cut, "the parts after the exports are the content, cut at every 7616 bits");
    bool one_open = true;
    for (size_t i = 0; i < seen.size(); ++i) one_open = one_open && (seen[i].b.is_open == (i == 0));
    CHECK(one_open, "bOpen is on the first part only");
    CHECK(seen.front().payload == bits_of(gs.exports[0]), "and the first part is the export bunch alone");
}

static void test_export_parts() {
    std::printf("ch 14: pkt#62/#63, two export bunches\n");
    Connection c(server_config(), t0);
    Chain ch14;
    ch14.channel = 14;
    ch14.open = true;
    ch14.exports.push_back(random_bits(7329, 5));
    ch14.exports.push_back(random_bits(961, 6));
    ch14.payload = random_bits(6804, 7);
    CHECK(c.send_chain(ch14), "an open with two export bunches");
    const auto pkts = c.flush(t0);
    const auto seen = parse(pkts);
    CHECK(seen.size() == 3 && pkts.size() == 2 && seen[1].packet == 1 && seen[2].packet == 1,
          "the second export part and the content share a packet, as in pkt#63");
    CHECK_CAPTURE(seen, 0, kCh14, "chSeq 954..956: the header bits are those of pkt#62/#63");
    CHECK(seen.size() == 3 && seen[0].b.has_package_map_exports && seen[1].b.has_package_map_exports &&
              !seen[2].b.has_package_map_exports && seen[0].b.partial_initial && !seen[1].b.partial_initial &&
              !seen[1].b.partial_final && seen[2].b.partial_final,
          "exports on both export parts, initial on the first part only, final on the last only");
}
// (CoA : C-Y-B)

static void test_channel_lifetime() {
    std::printf("ch 21: opens and closes, pkt#100..#15979\n");
    Connection c(server_config(), t0);
    Connection cl(client_config(), t0);
    Datagrams all;
    Clock::time_point t = t0;
    // A round of the real pair: the server's packets reach the client and the client's acks come back.  The
    // original server reopens an index a long time after the close (pkt#8332 to #8535), the acks are in by then.
    auto run = [&]() {
        t += milliseconds(100);
        for (auto& p : c.flush(t)) {
            all.push_back(p);
            cl.receive(p, t);
        }
        t += milliseconds(100);
        return_acks(c, cl, t);
    };

    CHECK(c.send_chain(chain(21, 5761, 7541, true)), "open with exports (pkt#100, #101)");
    run();
    CHECK(c.close_channel(21, kCloseRelevancy), "close with reason 3 (pkt#8332)");
    CHECK(!c.channel_open(21), "the channel is closed at once");
    CHECK(!c.send_chain(chain(21, 0, 100)), "nothing goes on a closed channel");
    CHECK(c.channel_closing(21) && !c.send_chain(chain(21, 0, 2422, true)),
          "and it cannot be opened before the close is acknowledged");
    run();
    CHECK(!c.channel_closing(21), "the close is acknowledged: the index is free");
    CHECK(c.send_chain(chain(21, 0, 2422, true)), "opened again with no exports, whole (pkt#8535)");
    run();
    CHECK(c.close_channel(21, kCloseDormancy), "closed with reason 1 (pkt#8539)");
    run();
    CHECK(c.send_chain(chain(21, 0, 7616 + 5354, true)), "opened with 12,970 bits of content, no exports (pkt#15976)");
    run();
    CHECK(c.close_channel(21, kCloseDormancy), "closed (pkt#15979)");
    run();

    const auto seen = parse(all);
    CHECK(seen.size() == 8, "eight bunches");
    CHECK_CAPTURE(seen, 0, kCh21Open, "the first open: chSeq 954/955, as pkt#100/#101");
    CHECK_CAPTURE(seen, 2, kCh21Close3, "the close is chSeq 956, reason 3, 0 bits, EName 102, as pkt#8332");
    CHECK_CAPTURE(seen, 3, kCh21Whole, "the second open goes on with 957 and is one bunch, as pkt#8535");
    CHECK_CAPTURE(seen, 4, kCh21Close1a, "the next close is 958, reason 1, as pkt#8539");
    CHECK_CAPTURE(seen, 5, kCh21Split, "an open without exports splits with bOpen on its first part: 959/960, as pkt#15976/#15977");
    CHECK_CAPTURE(seen, 7, kCh21Close1b, "and the close is 961, as pkt#15979");
    CHECK(seen.size() == 8 && seen[2].b.is_close && seen[2].b.is_control && !seen[2].b.is_open && seen[2].b.close_reason == 3 &&
              !seen[2].b.is_partial && seen[2].b.bunch_data_bits == 0,
          "a close is bControl and bClose with its reason, not partial, no data");
}

static void test_unreliable_chain() {
    std::printf("ch 3: pkt#149, an unreliable chain\n");
    Connection c(server_config(), t0);
    CHECK(c.send_chain(chain(3, 100, 100, true)), "channel 3 opened (954, 955)");
    Chain u = chain(3, 553, 1372);
    u.reliable = false;
    CHECK(c.send_chain(u), "an unreliable chain of 553 bits of exports and 1372 of content");
    CHECK(c.unacked_reliable(3) == 2, "it is not counted in flight");
    Chain open_unreliable = chain(4, 0, 10, true);
    open_unreliable.reliable = false;
    CHECK(!c.send_chain(open_unreliable) && !c.channel_open(4), "an open that is not reliable is refused");
    CHECK(c.send_chain(chain(3, 0, 10)), "the next reliable bunch");
    const auto seen = parse(c.flush(t0));
    CHECK(seen.size() == 5, "five bunches");
    CHECK_CAPTURE(seen, 2, kUnreliable, "the unreliable parts have the header bits of pkt#149: no chSeq, no channel name");
    CHECK(seen.size() == 5 && !seen[2].b.is_reliable && seen[2].b.is_partial && seen[2].b.partial_initial &&
              seen[2].b.has_package_map_exports && seen[3].b.partial_final,
          "exports first, partial initial then final");
    CHECK(seen.size() == 5 && seen[4].b.ch_sequence == 956, "and the reliable sequence went on with 956");
}

static void test_reassembly() {
    std::printf("the client reassembles a split chain\n");
    Connection s(server_config(), t0);
    Connection cl(client_config(), t0);

    // Content only: what the client's Connection reassembles is the content.
    Chain plain = chain(9, 0, 61000, true, 3);
    CHECK(s.send_chain(plain), "61,000 bits of content, no exports (9 parts)");
    auto got = deliver(cl, s.flush(t0), t0);
    CHECK(got.size() == 1 && got[0].channel == 9 && got[0].open && got[0].reliable && !got[0].close &&
              got[0].bits == 61000 && bits_of(got[0]) == bits_of(plain.payload),
          "one bunch comes out: the original 61,000 bits, opened");

    // With exports: the client reads each export bunch on its own; worldd's
    // reassembly does not know them, so it holds the export bits in front.
    Chain with_exports = chain(9, 0, 0, false, 9);
    with_exports.exports.push_back(random_bits(873, 10));
    with_exports.payload = random_bits(20000, 11);
    CHECK(s.send_chain(with_exports), "873 bits of exports and 20,000 of content");
    return_acks(s, cl, t0 + milliseconds(100));
    got = deliver(cl, s.flush(t0 + milliseconds(110)), t0 + milliseconds(110));
    const auto expect = concat(bits_of(with_exports.exports[0]), bits_of(with_exports.payload));
    CHECK(got.size() == 1 && !got[0].open && bits_of(got[0]) == expect, "one bunch comes out: the exports and the content, in order");

    // The close arrives as a bunch of its own.
    CHECK(s.close_channel(9, kCloseDestroyed), "closing the channel");
    got = deliver(cl, s.flush(t0 + milliseconds(120)), t0 + milliseconds(120));
    CHECK(got.size() == 1 && got[0].channel == 9 && got[0].close && got[0].bits == 0 && got[0].close_reason == 0,
          "and the client gets the close");
}

static void test_lost_part() {
    std::printf("a lost part is sent again\n");
    Connection s(server_config(), t0);
    Connection cl(client_config(), t0);
    Chain c = chain(5, 0, 0, true, 21);
    c.exports.push_back(random_bits(873, 22));
    c.payload = random_bits(20000, 23);  // 7616 + 7616 + 4768
    CHECK(s.send_chain(c), "a chain of an export part and three content parts");
    const auto pkts = s.flush(t0);
    const auto first = parse(pkts);
    CHECK(pkts.size() == 4 && first.size() == 4, "four parts in four packets");
    // The packet with the second content part (chSeq 956) is lost.
    auto got = deliver(cl, pkts, t0, 2);
    CHECK(got.empty(), "the client holds the chain: a part is missing");
    return_acks(s, cl, t0 + milliseconds(100));
    CHECK(s.unacked_reliable(5) == 1, "the server knows three parts arrived; one is still in flight");
    const auto again = s.flush(t0 + milliseconds(110));
    const auto resent = parse(again);
    CHECK(again.size() == 1 && resent.size() == 1 && resent[0].b.ch_sequence == 956 && resent[0].header == first[2].header &&
              resent[0].payload == first[2].payload,
          "it sends that part again: chSeq 956, the same bits");
    got = deliver(cl, again, t0 + milliseconds(110));
    const auto expect = concat(bits_of(c.exports[0]), bits_of(c.payload));
    CHECK(got.size() == 1 && bits_of(got[0]) == expect, "the client then reassembles the chain in order");
    return_acks(s, cl, t0 + milliseconds(200));
    CHECK(s.unacked_reliable(5) == 0, "and everything is acknowledged");
    CHECK(parse(s.flush(t0 + milliseconds(210))).empty(), "and no more copies of any part");
}

static void test_limits() {
    std::printf("sizes and the window\n");
    Connection c(server_config(), t0);
    auto sizes = [&](Datagrams pkts) {
        std::vector<size_t> v;
        for (const auto& s : parse(pkts)) v.push_back(s.b.bunch_data_bits);
        return v;
    };
    CHECK(c.send_chain(chain(2, 0, 7621, true)), "7621 bits of content");
    CHECK(sizes(c.flush(t0)) == std::vector<size_t>{7621}, "go out whole");
        CHECK(c.send_chain(chain(2, 0, 7622)), "7622 bits");
    CHECK(sizes(c.flush(t0)) == (std::vector<size_t>{7616, 6}), "go out as 7616 and 6");
    CHECK(c.send_chain(chain(2, 0, 2 * 7616)), "twice 7616 bits");
    CHECK(sizes(c.flush(t0)) == (std::vector<size_t>{7616, 7616}), "go out as two parts, no empty third");
    CHECK(c.send_chain(chain(2, 7621, 10)), "an export bunch of 7621 bits");
    CHECK(sizes(c.flush(t0)) == (std::vector<size_t>{7621, 10}), "is one part of its own");
    CHECK(!c.send_chain(chain(2, 7622, 10)), "one of 7622 bits is refused");
    CHECK(c.send_chain(chain(2, 0, Connection::kMaxPartialBits)), "524,288 bits of content (the client's limit)");
    CHECK(sizes(c.flush(t0)).size() == 69, "go out in 69 parts");
    CHECK(!c.send_chain(chain(2, 0, Connection::kMaxPartialBits + 1)), "one bit more is refused: the client would fail the connection");
    CHECK(c.unacked_reliable(2) == 1 + 2 + 2 + 2 + 69, "and the refusals left nothing in flight (76 parts queued)");

    // The window: kMaxUnackedReliable bunches on one channel.
    Connection w(server_config(), t0);
    CHECK(w.send_chain(chain(9, 0, 400, true)), "open channel 9");
    bool small_ok = true;
    for (int i = 0; i < 240; ++i) small_ok = small_ok && w.send_chain(chain(9, 0, 400));
    CHECK(small_ok && w.unacked_reliable(9) == 241, "240 small bunches: 241 in flight");
    CHECK(!w.send_chain(chain(9, 0, 15 * 7616 + 1000)), "a chain of 16 parts would make 257: refused whole");
    CHECK(w.unacked_reliable(9) == 241, "with nothing of it queued");
    CHECK(w.send_chain(chain(9, 0, 14 * 7616 + 1000)), "a chain of 15 parts makes exactly 256");
    CHECK(!w.send_chain(chain(9, 0, 400)) && !w.close_channel(9) && w.channel_open(9),
          "the window is full: one more bunch and the close are refused, the channel stays open");
    const Datagrams pkts = w.flush(t0);
    const auto seen = on_channel(parse(pkts), 9);
    bool consecutive = seen.size() == 256;
    for (size_t i = 0; i < seen.size(); ++i) consecutive = consecutive && seen[i].b.ch_sequence == (954 + i) % 1024;
    CHECK(consecutive, "256 bunches went out, chSeq 954.. consecutive modulo 1024 (none of the refused chain)");
}
// (C | Y | B | CoA)

static void test_channel_state() {
    std::printf("channel state\n");
    Connection c(server_config(), t0);
    Connection cl(client_config(), t0);
    Datagrams sent;   // everything the server sent, in order
    CHECK(!c.send_chain(chain(5, 0, 100)) && !c.channel_open(5), "a chain on a channel that is not open is refused");
    CHECK(!c.send_chain(chain(0, 0, 100, true)) && !c.send_chain(chain(0, 0, 100)), "channel 0 is not an actor channel");
    CHECK(c.send_chain(chain(5, 0, 100, true)) && c.channel_open(5), "the opening chain is accepted");
    CHECK(!c.send_chain(chain(5, 0, 100, true)), "a second open of an open channel is refused");
    CHECK(c.send_chain(chain(5, 0, 100)), "a chain on the open channel is accepted");
    CHECK(!c.close_channel(7) && !c.close_channel(0), "closing a channel that is not open is refused");
    CHECK(!c.close_channel(5, Connection::kCloseReasonSpace) && c.channel_open(5), "a close reason of 15 or more is refused");
    CHECK(c.close_channel(5, kCloseTearOff) && !c.channel_open(5), "the close is accepted");
    CHECK(!c.close_channel(5), "a channel closes once");
    CHECK(!c.send_chain(chain(5, 0, 100)), "nothing goes on it after");
    CHECK(!c.send_chain(chain(5, 0, 100, true)) && c.channel_closing(5) && !c.channel_open(5),
          "and it cannot be opened again while its close is not acknowledged");
    for (auto& p : c.flush(t0)) {
        sent.push_back(p);
        cl.receive(p, t0);
    }
    return_acks(c, cl, t0 + milliseconds(100));
    CHECK(!c.channel_closing(5), "the client acknowledges the close");
    CHECK(c.send_chain(chain(5, 0, 100, true)) && c.channel_open(5), "and then it can be opened again");
    for (auto& p : c.flush(t0 + milliseconds(110))) sent.push_back(p);
    auto seen = on_channel(parse(sent), 5);
    CHECK(seen.size() == 4 && seen[0].b.ch_sequence == 954 && seen[1].b.ch_sequence == 955 && seen[2].b.ch_sequence == 956 &&
              seen[3].b.ch_sequence == 957 && seen[2].b.is_close && seen[2].b.close_reason == 4 && seen[3].b.is_open,
          "the queue has open 954, bunch 955, close 956 (reason 4), open 957: the refused ones left no gap");

    // The control channel still works beside them, on its own sequence.
    conn::BunchHeader control;
    control.reliable = true;
    control.channel = 0;
    emit::BunchWriter one;
    one.write_uint8(1);
    CHECK(c.send(control, one) && c.send_close(), "channel 0 keeps using send");
    seen = on_channel(parse(c.flush(t0 + milliseconds(120))), 0);
    CHECK(seen.size() == 2 && seen[0].b.ch_sequence == 954 && seen[1].b.ch_sequence == 955 && seen[1].b.is_close,
          "with its own numbers, 954 and 955");
}

// -- not in the capture: the rules of UE the Connection follows ---------------------------------------

/// The real pair for the tests below: the server's packets go to the client (one of a batch may be lost on the
/// way), the client's acks come back.  Everything the server sent is kept, and what the client's Connection
/// delivered.
struct Pair {
    Connection s{server_config(), t0};
    Connection cl{client_config(), t0};
    Clock::time_point t = t0;
    Datagrams sent;
    std::vector<InBunch> at_client;

    /// Flushes the server; the packets reach the client, except index `drop`; the acks return.  Returns the batch.
    Datagrams step(size_t drop = SIZE_MAX) {
        t += milliseconds(100);
        Datagrams pkts = s.flush(t);
        for (const auto& p : pkts) sent.push_back(p);
        for (auto& b : deliver(cl, pkts, t, drop)) at_client.push_back(std::move(b));
        t += milliseconds(100);
        return_acks(s, cl, t);
        return pkts;
    }
};

static void test_closing_index() {
    std::printf("a closed index is taken until the close is acknowledged (UChannel::ReceivedAcks)\n");
    {
        Pair p;
        CHECK(p.s.send_chain(chain(21, 0, 100, true)), "channel 21 opens (chSeq 954)");
        p.step();
        CHECK(p.s.close_channel(21, kCloseRelevancy), "the close is queued (955)");
        CHECK(p.s.channel_closing(21) && !p.s.channel_open(21), "the channel is closing, not open");
        CHECK(!p.s.send_chain(chain(21, 0, 100, true)), "an open of the index is refused at once");
        const auto pkts = p.s.flush(p.t + milliseconds(10));
        CHECK(pkts.size() == 1 && !p.s.send_chain(chain(21, 0, 100, true)) && p.s.channel_closing(21),
              "and still when the close is on the wire");
        for (const auto& pk : pkts) {
            p.sent.push_back(pk);
            for (auto& b : p.cl.receive(pk, p.t + milliseconds(10))) p.at_client.push_back(std::move(b));
        }
        CHECK(p.s.channel_closing(21), "delivered is not acknowledged: still closing");
        return_acks(p.s, p.cl, p.t + milliseconds(200));
        CHECK(!p.s.channel_closing(21), "the client acknowledges the packet: the index is free");
        CHECK(p.s.send_chain(chain(21, 0, 100, true)) && p.s.channel_open(21), "an open is accepted");
        p.step();
        const auto seen = on_channel(parse(p.sent), 21);
        CHECK(seen.size() == 3 && seen[0].b.is_open && seen[1].b.is_close && seen[2].b.is_open && seen[1].b.ch_sequence == 955 &&
                  seen[2].b.ch_sequence == 956,
              "open 954, close 955, open 956: the reopen follows the close's sequence");
        CHECK(p.at_client.size() == 3 && p.at_client[0].open && p.at_client[1].close && p.at_client[2].open,
              "the client gets them in that order");
    }
    {   // the failure this prevents: the close's packet is lost, the open of the same index arrives first, then the
        // resent close runs CleanUp on the new channel (DataChannel.cpp:302-340, 454-540): the actor never appears
        Pair p;
        CHECK(p.s.send_chain(chain(21, 0, 100, true)), "channel 21 opens (954)");
        p.step();
        CHECK(p.s.close_channel(21, kCloseDormancy), "the close (955) ...");
        p.step(0);   // ... is lost
        CHECK(p.at_client.size() == 1 && p.s.channel_closing(21), "the client never saw it; the channel is closing");
        CHECK(!p.s.send_chain(chain(21, 0, 100, true)), "an open of the index is refused");
        CHECK(p.s.send_chain(chain(22, 0, 100, true)), "another channel opens (954)");
        p.step();   // its packet arrives: the client's acks show the close's packet missing
        CHECK(p.at_client.size() == 2 && p.at_client[1].channel == 22, "the client gets channel 22's open, not the close");
        CHECK(p.s.channel_closing(21) && !p.s.send_chain(chain(21, 0, 100, true)),
              "the close's packet is reported lost and the close queued again: the index is still taken");
        const auto again = p.step();   // the close again, the same sequence
        const auto resent = on_channel(parse(again), 21);
        CHECK(resent.size() == 1 && resent[0].b.is_close && resent[0].b.ch_sequence == 955, "the resent close has chSeq 955");
        CHECK(!p.s.channel_closing(21), "it is acknowledged: the index is free");
        CHECK(p.s.send_chain(chain(21, 0, 100, true)), "the open is accepted (956)");
        p.step();
        std::vector<const InBunch*> on21;
        for (const auto& b : p.at_client)
            if (b.channel == 21) on21.push_back(&b);
        CHECK(on21.size() == 3 && on21[0]->open && on21[1]->close && on21[1]->close_reason == kCloseDormancy && on21[2]->open,
              "the client sees open, close, open: the close always reaches it before the reopen");
        const auto seen = on_channel(parse(p.sent), 21);
        CHECK(seen.size() == 4 && seen[3].b.is_open && seen[3].b.ch_sequence == 956, "the reopen is 956 (the close was sent twice)");
    }
}

static void test_ack_events() {
    std::printf("the acks of a chain's parts: AckEvents\n");
    Pair p;
    Chain c = chain(5, 873, 20000, true, 31);   // the export, then 7616 + 7616 + 4768 of content: four parts
    const ChainId id = p.s.send_chain(c);
    CHECK(id != 0 && p.s.take_ack_events().empty(), "send_chain returns the chain's id; nothing is acknowledged yet");
    Chain refused = chain(5, 0, 100, true);   // the channel is open now
    CHECK(p.s.send_chain(refused) == 0, "a refused chain has no id");
    const ChainId other = p.s.send_chain(chain(6, 0, 100, true));
    CHECK(other != 0 && other != id, "every accepted chain has an id of its own");
    const auto pkts = p.step(2);   // the packet of the second content part is lost
    CHECK(pkts.size() == 4, "four packets: the export, two parts of 7616 bits, the last part with the other chain's");
    auto ev = p.s.take_ack_events();
    // packets in order: 0 the export, 1 and 2 content, 3 the last content part and channel 6's open; index 2 was lost
    CHECK(ev.size() == 5, "five events: four parts delivered and one lost");
    auto at = [&](size_t i) { return i < ev.size() ? ev[i] : AckEvent{}; };
    CHECK(at(0).chain == id && at(0).part == 0 && at(0).export_part && at(0).delivered && at(0).channel == 5,
          "part 0 is the export, delivered");
    CHECK(at(1).chain == id && at(1).part == 1 && !at(1).export_part && at(1).delivered, "part 1 is content, delivered");
    CHECK(at(2).chain == id && at(2).part == 2 && !at(2).export_part && !at(2).delivered, "part 2 was lost");
    CHECK(at(3).chain == id && at(3).part == 3 && at(3).delivered && at(4).chain == other && at(4).part == 0 && at(4).delivered,
          "part 3 and the other chain's part were delivered");
    CHECK(p.s.take_ack_events().empty(), "the events are taken once");
    p.step();   // the lost part, sent again
    ev = p.s.take_ack_events();
    CHECK(ev.size() == 1 && ev[0].chain == id && ev[0].part == 2 && ev[0].delivered,
          "the lost reliable part is sent again and its delivery is reported when that packet is acknowledged");
    CHECK(p.at_client.size() == 2 && p.at_client[1].channel == 5 && p.at_client[1].bits == 873 + 20000,
          "and the client has the whole chain (channel 6's open came before)");
// CoA and C Y B

    // An unreliable chain: nothing sends a lost part again.
    Chain u = chain(5, 553, 1372, false, 41);
    u.reliable = false;
    const ChainId uid = p.s.send_chain(u);
    p.step(0);   // lost (both parts are in its one packet)
    CHECK(p.s.take_ack_events().empty(), "an unreliable chain's loss is not known until a later packet is acknowledged");
    CHECK(p.s.send_chain(chain(6, 0, 10)) != 0, "a later packet");
    p.step();
    ev = p.s.take_ack_events();
    CHECK(ev.size() == 3 && ev[0].chain == uid && ev[0].export_part && !ev[0].delivered && ev[1].chain == uid && ev[1].part == 1 &&
              !ev[1].delivered,
          "both parts of the lost unreliable chain are reported lost (the export first)");
    bool resent = false;
    for (const auto& s : on_channel(parse(p.step()), 5)) resent = resent || !s.b.is_reliable;
    CHECK(!resent, "and are not sent again");
    Chain u2 = chain(5, 100, 100, false, 42);
    u2.reliable = false;
    const ChainId uid2 = p.s.send_chain(u2);
    p.step();
    ev = p.s.take_ack_events();
    CHECK(ev.size() == 2 && ev[0].chain == uid2 && ev[0].delivered && ev[1].chain == uid2 && ev[1].delivered,
          "an unreliable chain that arrives is reported delivered");

    // A close is no chain: its ack frees the index and is no event.
    CHECK(p.s.close_channel(5) && p.s.channel_closing(5), "a close");
    p.step();
    CHECK(!p.s.channel_closing(5) && p.s.take_ack_events().empty(), "acknowledged: the index is free, no AckEvent");
}

// can_send_chain and send_chain agree, and the first changes nothing.
static void agree(Connection& c, const Chain& chain_, bool expect, const char* msg) {
    const size_t unacked = c.unacked_reliable(chain_.channel);
    const bool was_open = c.channel_open(chain_.channel);
    const bool was_closing = c.channel_closing(chain_.channel);
    const bool can = c.can_send_chain(chain_);
    const bool can_again = c.can_send_chain(chain_);
    const bool untouched = c.unacked_reliable(chain_.channel) == unacked && c.channel_open(chain_.channel) == was_open &&
                           c.channel_closing(chain_.channel) == was_closing;
    const bool sent = c.send_chain(chain_) != 0;
    const bool ok = can == expect && can_again == can && untouched && sent == can;
    if (!ok) std::printf("    can=%d again=%d untouched=%d sent=%d expected=%d\n", can, can_again, untouched, sent, expect);
    CHECK(ok, msg);
}

static void test_can_send_chain() {
    std::printf("can_send_chain: send_chain's checks, nothing queued\n");
    Pair p;
    Connection& c = p.s;
    agree(c, chain(0, 0, 10, true), false, "channel 0 is refused");
    agree(c, chain(5, 0, 10), false, "a chain on a channel that is not open is refused");
    Chain unreliable_open = chain(5, 0, 10, true);
    unreliable_open.reliable = false;
    agree(c, unreliable_open, false, "an open that is not reliable is refused");
    agree(c, chain(5, 7622, 10, true), false, "an export bunch of 7622 bits is refused");
    agree(c, chain(5, 0, Connection::kMaxPartialBits + 1, true), false, "content over 524,288 bits is refused");
    agree(c, chain(5, 0, 10, true), true, "an open of a free index is accepted");
    agree(c, chain(5, 0, 10, true), false, "an open of an open channel is refused");
    agree(c, chain(5, 0, 10), true, "a chain on the open channel is accepted");
    CHECK(c.close_channel(5), "the channel is closed");
    agree(c, chain(5, 0, 10, true), false, "an open of a closing index is refused");
    p.step();
    agree(c, chain(5, 0, 10, true), true, "and accepted once the close is acknowledged");
    // a chain of 257 parts can never fit (188 export bunches of 8 bits and 69 parts of content, 64 KB in all, the
    // reassembly's limit) ...
    Chain huge = chain(6, 0, Connection::kMaxPartialBits - 188 * 8, true);
    for (int i = 0; i < 188; ++i) huge.exports.push_back(random_bits(8, 50 + i));
    agree(c, huge, false, "a chain of 257 parts is refused, however empty the window");
    // ... and one of 256 fits an empty window exactly
    Chain max = chain(6, 0, Connection::kMaxPartialBits - 187 * 8, true);
    for (int i = 0; i < 187; ++i) max.exports.push_back(random_bits(8, 50 + i));
    agree(c, max, true, "and one of 256 is accepted");
    CHECK(c.unacked_reliable(6) == 256, "it fills the window of channel 6");
    agree(c, chain(6, 0, 10), false, "the window is full: one more is refused");
    Chain unreliable = chain(6, 0, 10);
    unreliable.reliable = false;
    agree(c, unreliable, true, "an unreliable chain is not counted in the window");
    p.step();
    p.step();
    agree(c, chain(6, 0, 10), true, "the acks free the window");

    // It queues nothing and takes no id: a twin connection that was asked gives the same ids and the same bytes.
    Connection asked(server_config(), t0), twin(server_config(), t0);
    const Chain open = chain(9, 873, 12000, true, 77);
    CHECK(asked.can_send_chain(open) && asked.can_send_chain(open) && asked.flush(t0).empty() && !asked.channel_open(9) &&
              asked.unacked_reliable(9) == 0,
          "asked twice: nothing is queued, the channel is not open, nothing in flight");
    const ChainId a = asked.send_chain(open);
    const ChainId b = twin.send_chain(open);
    CHECK(a != 0 && a == b, "and the chain gets the id it would have got unasked");
    CHECK(asked.flush(t0) == twin.flush(t0), "and the same datagrams go out");
}

// While the packet window is full nothing goes out, and unreliable bunches queue up.  UE drops unreliable data it
// cannot send; the Connection keeps at most kMaxQueuedUnreliable of them and drops the oldest, a whole unreliable
// chain at a time (its other parts are of no use alone).  Reliable bunches are never dropped.
static void test_unreliable_queue_bound() {
    std::printf("unreliable bunches that cannot go out: a bounded queue\n");
    const size_t bound = Connection::kMaxQueuedUnreliable;
    Connection s(server_config(), t0);
    fake::Client cl(kOuter, kCf, 14265, 15870);
    s.receive(cl.packet({cl.hello()}), t0);
    std::vector<std::vector<uint8_t>> kas;  // the client is silent: keep-alives until the window of 256 is full
    for (int i = 1; i <= 300; ++i)
        for (auto& p : s.flush(t0 + milliseconds(200 * i))) kas.push_back(std::move(p));
    CHECK(s.notify().is_window_full(), "the client is silent and the packet window is full");
    const auto now = t0 + seconds(100);
// C.O.A.C_Y_B

    CHECK(s.send_chain(chain(5, 0, 100, true)) != 0, "a reliable open of channel 5 is queued behind the window");
    // bound + 100 unreliable chains of two parts each (an export of 100 bits and 200 + i of content), and a
    // reliable chain of 77 bits among them.
    std::vector<ChainId> ids;
    for (size_t i = 0; i < bound + 100; ++i) {
        if (i == 150) CHECK(s.send_chain(chain(5, 0, 77)) != 0, "a reliable chain is queued among them");
        Chain u = chain(5, 100, 200 + i, false, static_cast<uint32_t>(i + 1));
        u.reliable = false;
        ids.push_back(s.send_chain(u));
    }
    const size_t queued_parts = 2 * (bound + 100);
    CHECK(s.flush(now).empty(), "nothing goes out while the window is full");
    CHECK(s.queued_unreliable() <= bound && s.queued_unreliable() + 1 >= bound,
          "the unreliable queue is cut to its bound (256 bunches)");
    CHECK(s.unreliable_dropped() == queued_parts - s.queued_unreliable(), "the rest were dropped");
    CHECK(s.unacked_reliable(5) == 2, "both reliable bunches are still queued: the open and the 77-bit chain");
    auto ev = s.take_ack_events();
    bool all_lost = ev.size() == s.unreliable_dropped(), oldest_first = true;
    for (size_t i = 0; i < ev.size(); ++i) {
        all_lost = all_lost && !ev[i].delivered;
        oldest_first = oldest_first && ev[i].chain == ids[i / 2] && ev[i].part == i % 2 && ev[i].export_part == (i % 2 == 0);
    }
    CHECK(all_lost && oldest_first, "each dropped part is reported lost (a lost unreliable part is final), the oldest chains first");

    // Plain unreliable bunches count too, and push the chains out.
    conn::BunchHeader h;
    h.channel = 5;
    h.reliable = false;
    for (size_t i = 0; i < bound + 44; ++i) {
        emit::BunchWriter w;
        w.write_bits(i, 33);
        CHECK_QUIET(s.send(h, w));
    }
    CHECK(s.flush(now + milliseconds(1)).empty() && s.queued_unreliable() == bound,
          "the queue stays at its bound: 256 plain unreliable bunches queued, the older chains' parts dropped");
    CHECK(s.unacked_reliable(5) == 2, "the reliable ones are still there");

    // The window opens: the two reliable bunches go out in order, with the newest unreliable ones.
    for (const auto& p : kas) cl.receive(p);
    CHECK(s.receive(cl.packet({cl.netspeed(7)}), now + seconds(1)).size() == 1 && !s.notify().is_window_full(),
          "the client's next packet opens the window");
    const auto seen = on_channel(parse(s.flush(now + seconds(1) + milliseconds(60))), 5);
    std::vector<unsigned> reliable_seqs;
    size_t unreliable_out = 0, stale = 0;
    for (const auto& b : seen) {
        if (b.b.is_reliable) reliable_seqs.push_back(b.b.ch_sequence);
        else {
            ++unreliable_out;
            if (b.payload.size() != 33) ++stale;
        }
    }
    CHECK(reliable_seqs.size() == 2 && reliable_seqs[0] == 954 && reliable_seqs[1] == 955,
          "the open (954) and the reliable chain (955) were sent, in order");
    CHECK(unreliable_out == bound && stale == 0 && s.queued_unreliable() == 0,
          "and the 256 newest unreliable bunches, none of the dropped chains' parts");
}

int main() {
    test_pc_channel();
    test_gamestate_open();
    test_export_parts();
    test_channel_lifetime();
    test_unreliable_chain();
    test_reassembly();
    test_lost_part();
    test_limits();
    test_channel_state();
    test_closing_index();
    test_ack_events();
    test_can_send_chain();
    test_unreliable_queue_bound();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
