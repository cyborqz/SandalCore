// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_move_packed.cpp
//
//  protocol/repl/move_packed.{h,cpp} against every ServerMovePacked the retail client sent in the two
//  captures (capture_moves.h: 640 of the e2e capture, 1,843 of the 2026-09-29 native run):
//
//    every parameter reads, and ends exactly where its NumBits says (the reader refuses leftovers);
//    TimeStamps of the new moves rise through each capture and a pending move's lies between the
//      previous new move's and its own new move's;
//    consecutive locations are continuous: no step is faster than the fastest the captures show
//      (the pawn's dodge, 1,423 cm/s) with a margin;
//    the counts the layout predicts: 91 / 158 moves carry a pending move, none an old one;
//    writing a read move back gives the client's own bits, all 2,483 times;
//    the spawn of the e2e capture, field by field.
//
//  Hand-made moves cover what no capture holds: a movement base and its bone, an old move, the optional
//  values, and the refusals (a zeroed struct, NumBits over the exe's 4096 or not where the parameter
//  ends, leftover bits, a truncated move, a base the reader cannot read).
//
//  The reader is handed its buffer's size and never reads past it (an `end` beyond the buffer is refused);
//  a seeded fuzz (40,000 reads of mutated captures, random containers and hostile NumBits, each from a heap
//  buffer of exactly the bytes it needs, so an address sanitizer build reports an over-read) checks that
//  every capture reads and whatever reads writes back to bits that read and write to themselves.
// ============================================================================
#include "capture_moves.h"
#include "protocol/repl/move_packed.h"
#include "protocol/wire/packet_reader.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
namespace wire = aoc::protocol::wire;
using emit::BunchWriter;
using emit::FIntrepidNetworkGUID;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static std::vector<uint8_t> bytes_of(const char* hex) {
    std::vector<uint8_t> out;
    auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) out.push_back(static_cast<uint8_t>(nibble(hex[i]) << 4 | nibble(hex[i + 1])));
    return out;
}

static bool same_bit_range(const uint8_t* a, const uint8_t* b, size_t bits) {
    for (size_t i = 0; i < bits; ++i)
        if (((a[i >> 3] >> (i & 7)) & 1) != ((b[i >> 3] >> (i & 7)) & 1)) return false;
    return true;
}
// (C_Y_B + C-O-A)

struct Stats {
    size_t moves = 0, read = 0, pending = 0, old = 0, dual = 0, disable = 0, round_trip = 0;
    size_t not_rising = 0, pending_out_of_order = 0, jumps = 0, modes[256] = {};
    double fastest = 0;
};

template <size_t N>
static Stats read_all(const capture_moves::Move (&moves)[N]) {
    Stats s;
    bool have_prev = false;
    NetworkMove prev;
    for (const capture_moves::Move& mv : moves) {
        ++s.moves;
        const auto data = bytes_of(mv.hex);
        const auto m = read_move_packed(data.data(), data.size(), 0, mv.bits);
        if (!m) continue;
        ++s.read;
        s.pending += m->pending.has_value();
        s.old += m->old.has_value();
        s.dual += m->dual_hybrid_root_motion;
        s.disable += m->disable_combined_scoped_move;
        ++s.modes[m->new_move.movement_mode];
        BunchWriter w;
        if (write_move_packed(w, *m) && w.bit_pos() == mv.bits && same_bit_range(w.data(), data.data(), mv.bits)) ++s.round_trip;
        if (have_prev) {
            if (!(m->new_move.time_stamp > prev.time_stamp)) ++s.not_rising;
            if (m->pending && !(m->pending->time_stamp > prev.time_stamp && m->pending->time_stamp < m->new_move.time_stamp)) ++s.pending_out_of_order;
            // the step from the previous new move to this one, or from the pending move when there is one
            const NetworkMove& from = prev;
            const double dt = m->new_move.time_stamp - from.time_stamp;
            const Vec3d a = from.location_cm(), b = m->new_move.location_cm();
            const double h = std::hypot(b.x - a.x, b.y - a.y);
            // a gap of more than a second is a pause (loading, a stall): no speed is read from it
            if (dt > 0.005 && dt < 1.0) s.fastest = std::max(s.fastest, h / dt);
            if (h > 1500.0 * dt + 100.0) ++s.jumps;
        }
        prev = m->new_move;
        have_prev = true;
    }
    return s;
}

static void test_captures() {
    std::printf("every captured ServerMovePacked\n");
    const Stats e = read_all(capture_moves::kE2e);
    const Stats n = read_all(capture_moves::kNative);
    CHECK(e.moves == 640 && e.read == 640, "the e2e capture: 640 moves, all of them read, each to the last bit of its parameter");
    CHECK(n.moves == 1843 && n.read == 1843, "the 2026-09-29 run: 1,843 moves, all of them read");
    CHECK(e.round_trip == 640 && n.round_trip == 1843, "writing a read move back gives the client's own bits: all 2,483");
    CHECK(e.not_rising == 0 && n.not_rising == 0, "the TimeStamps of the new moves rise through each capture");
    CHECK(e.pending_out_of_order == 0 && n.pending_out_of_order == 0,
          "a pending move's TimeStamp is after the previous new move's and before its own new move's");
    CHECK(e.jumps == 0 && n.jumps == 0, "consecutive new moves are continuous: no step beyond 1,500 cm/s plus 100 cm");
    std::printf("    fastest step between new moves: e2e %.0f cm/s, native %.0f cm/s\n", e.fastest, n.fastest);
    CHECK(e.pending == 91 && n.pending == 158, "a pending move rides along in 91 of the e2e moves and 158 of the native run's");
    CHECK(e.old == 0 && n.old == 0, "no captured move carries an old move (the client never has two unacknowledged)");
    CHECK(e.dual == 1 && n.dual == 12, "bIsDualHybridRootMotionMove is set in 1 and 12 of them (a flag without root motion)");
    CHECK(e.disable > 0 && n.disable > 0, "bDisableCombinedScopedMove is set in some: the last bit of the container");
    CHECK(e.modes[kMoveWalking] > 0 && e.modes[kMoveFalling] > 0 && n.modes[kMoveWalking] > 0 && n.modes[kMoveFalling] > 0 &&
              e.modes[0] > 0 && n.modes[0] > 0,
          "movement modes seen: walking 1, falling 3, and 0 (none) before the pawn has landed");
    CHECK(n.fastest > 1200.0 && n.fastest < 1500.0, "the native run's fastest step is the dodge's, ~1,420 cm/s (spec section 10)");
}

static void test_first_move() {
    std::printf("the first move of the e2e capture\n");
    const auto data = bytes_of(capture_moves::kE2e[0].hex);
    const auto m = read_move_packed(data.data(), data.size(), 0, capture_moves::kE2e[0].bits);
    CHECK(m.has_value(), "it reads");
    if (!m) return;
    const NetworkMove& n = m->new_move;
    const Vec3d loc = n.location_cm();
    CHECK(std::fabs(n.time_stamp - 0.8125f) < 0.01f, "TimeStamp ~0.81 s");
    CHECK(loc.x == -777664.5 && std::fabs(loc.y - 616578.69) < 1e-6 && std::fabs(loc.z - 15952.05) < 1e-6,
          "Location (-777664.50, 616578.69, 15952.05): the character's spawn point, to 0.01 cm");
    CHECK(n.acceleration_cm().x == 0 && n.acceleration_cm().y == 0 && n.acceleration_cm().z == 0, "standing still: no acceleration");
    CHECK(n.control_rotation.yaw == 16550 && n.control_rotation.pitch == 0 && std::fabs(n.yaw_degrees() - 16550.0 * 360.0 / 65536.0) < 1e-9,
          "ControlRotation yaw 16550 (90.9 degrees), the pawn's spawn rotation");
    CHECK(n.compressed_flags == 0 && !n.base && !n.base_bone && n.movement_mode == 0 && n.input_x() == 0 && n.input_y() == 0 && !m->pending && !m->old,
          "no flags, no base, mode 0, no input, no pending or old move");
}

static NetworkMove walking_move(float ts, double x, double y, double z) {
    NetworkMove m;
    m.time_stamp = ts;
    m.acceleration = quantize_vector(10, 0, 0, 0);
    m.location = quantize_vector(100, x, y, z);
    m.control_rotation.yaw = 16384;
    return m;
}

static std::vector<uint8_t> written(const MovePacked& p, size_t* bits) {
    BunchWriter w;
    if (!write_move_packed(w, p)) {
        *bits = 0;
        return {};
    }
    *bits = w.bit_pos();
    return std::vector<uint8_t>(w.data(), w.data() + w.byte_size());
}
// C | O | A - Cyb

static void test_made_moves() {
    std::printf("moves nobody captured\n");
    MovePacked p;
    p.new_move = walking_move(12.5f, -1000.25, 2000.5, 30.75);
    p.new_move.acceleration = quantize_vector(10, 8189.0, -222.2, 0);
    p.new_move.compressed_flags = 0x11;
    p.new_move.movement_mode = kMoveFalling;
    p.new_move.input_bits = 0b0101;
    p.pending = walking_move(12.49f, -1000.0, 2000.0, 30.0);
    p.dual_hybrid_root_motion = true;
    p.old = walking_move(12.47f, -999.0, 1999.0, 30.0);
    p.old->control_rotation.pitch = 100;
    p.old->compressed_flags = 2;
    p.disable_combined_scoped_move = true;
    size_t bits = 0;
    auto data = written(p, &bits);
    std::string why;
    auto r = read_move_packed(data.data(), data.size(), 0, bits, &why);
    CHECK(bits > 0 && r && *r == p, "a container with all three moves, flags, a mode and an input direction round trips");
    CHECK(r && r->new_move.input_x() == 1 && r->new_move.input_y() == 1 && r->new_move.location_cm().x == -1000.25 &&
              r->new_move.acceleration_cm().x == 8189.0 && std::fabs(r->new_move.acceleration_cm().y + 222.2) < 1e-9,
          "its values: input (1, 1), Location x -1000.25 and Acceleration (8189.0, -222.2) at their quantization");
    CHECK(r && r->old && r->old->control_rotation.pitch == 100 && r->old->compressed_flags == 2 && r->old->time_stamp == 12.47f, "the old move is the third one on the wire");

    // a movement base and its bone
    MovePacked b;
    b.new_move = walking_move(1.0f, 0, 0, 0);
    FIntrepidNetworkGUID base;
    base.ObjectId = 0x1234567890;
    base.ServerId = 60;
    base.Randomizer = 0xdeadbeef;
    b.new_move.base = base;
    b.new_move.base_bone = NameValue::plain("Deck_Socket", 3);
    data = written(b, &bits);
    r = read_move_packed(data.data(), data.size(), 0, bits, &why);
    CHECK(r && *r == b && r->new_move.base && *r->new_move.base == base, "a move on a movement base: its GUID and its bone name round trip");
    b.new_move.base_bone = NameValue::hardcoded_name(7);
    data = written(b, &bits);
    r = read_move_packed(data.data(), data.size(), 0, bits, &why);
    CHECK(r && *r == b, "a hardcoded bone name too");

    // inputs: all four bits
    for (uint8_t in = 0; in < 16; ++in) {
        MovePacked q;
        q.new_move = walking_move(2.0f, 1, 2, 3);
        q.new_move.input_bits = in;
        data = written(q, &bits);
        r = read_move_packed(data.data(), data.size(), 0, bits);
        if (!r || r->new_move.input_bits != in) {
            CHECK(false, "all 16 input bit patterns round trip");
            return;
        }
    }
    CHECK(true, "all 16 input bit patterns round trip");
    NetworkMove both;
    both.input_bits = 0b0011;
    CHECK(both.input_x() == 1 && both.input_y() == 0, "x > 0 and x < 0 both set: the positive wins, as the exe's reader");
    both.input_bits = 0b1010;
    CHECK(both.input_x() == -1 && both.input_y() == -1, "x < 0, y < 0");

    // the minimal container: the exe's own sizes (4 bits of options, 3 of container, a 7-bit quantized zero ...)
    MovePacked z;
    z.new_move = walking_move(0.5f, 0, 0, 0);
    data = written(z, &bits);
    CHECK(bits > 40 && bits < 200, "a minimal container is a few dozen bits");
}

static void test_refusals() {
    std::printf("what the reader refuses\n");
    MovePacked p;
    p.new_move = walking_move(3.0f, 10, 20, 30);
    size_t bits = 0;
    const auto data = written(p, &bits);
    std::string why;
    CHECK(read_move_packed(data.data(), data.size(), 0, bits, &why).has_value(), "(the move to damage reads)");
    CHECK(!read_move_packed(data.data(), data.size(), 0, bits - 1, &why) && !why.empty(), "a parameter one bit short: refused, with a reason");
    CHECK(!read_move_packed(data.data(), data.size(), 0, 0, &why), "an empty parameter");
    CHECK(!read_move_packed(data.data(), data.size(), 0, 1), "a send bit and nothing else");

    // leftover bits: the same parameter with its NumBits one larger and a stray bit after the container
    wire::PacketReader pr(data.data(), data.size(), bits);
    pr.read_bit();
    const size_t num_bits = static_cast<size_t>(pr.read_sip().value_or(0));
    const size_t payload_at = pr.pos();
    BunchWriter leftover;
    leftover.write_bit(1);
    leftover.write_sip(num_bits + 1);
    leftover.write_bit_range(data.data(), payload_at, num_bits);
    leftover.write_bit(0);
    CHECK(num_bits > 0 && payload_at + num_bits == bits, "(the move's NumBits and where its container starts)");
    CHECK(!read_move_packed(leftover.data(), leftover.byte_size(), 0, leftover.bit_pos(), &why) && why.find("left") != std::string::npos,
          "a stray bit after the container: refused (the parameter must end where the exe's reader ends)");

    // NumBits that disagrees with where the parameter ends
    BunchWriter lies;
    lies.write_bit(1);
    lies.write_sip(num_bits + 40);
    lies.write_bit_range(data.data(), payload_at, num_bits);
    CHECK(!read_move_packed(lies.data(), lies.byte_size(), 0, lies.bit_pos(), &why) && why.find("NumBits") != std::string::npos, "NumBits larger than the bits that follow: refused");
// CoA captures, C.Y.B code

    // over the exe's limit, with the bits there
    BunchWriter big;
    big.write_bit(1);
    big.write_sip(kMaxPackedMoveBits + 1);
    for (uint32_t i = 0; i < kMaxPackedMoveBits + 1; ++i) big.write_bit(0);
    CHECK(!read_move_packed(big.data(), big.byte_size(), 0, big.bit_pos(), &why) && why.find("limit") != std::string::npos,
          "NumBits of 4,097: refused at the exe's limit (4,096), before any bit is read");

    // the send bit 0: a zeroed struct
    BunchWriter zeroed;
    zeroed.write_bit(0);
    CHECK(!read_move_packed(zeroed.data(), zeroed.byte_size(), 0, 1, &why) && why.find("zeroed") != std::string::npos, "send bit 0 (a zeroed struct): refused");

    // a base that is not a plain object reference (ObjectId 1 carries export flags the property reader does not read)
    MovePacked based;
    based.new_move = walking_move(4.0f, 1, 1, 1);
    FIntrepidNetworkGUID one;
    one.ObjectId = 1;
    based.new_move.base = one;
    BunchWriter w;
    CHECK(!write_move_packed(w, based) && w.bit_pos() == 0, "writing a base with ObjectId 1: refused, nothing written");
    based.new_move.base.reset();
    based.new_move.time_stamp = 5.0f;
    size_t b2 = 0;
    auto d2 = written(based, &b2);
    CHECK(read_move_packed(d2.data(), d2.size(), 0, b2).has_value(), "(the same move without the base reads)");

    // reading at an offset: the parameter inside a bigger buffer
    BunchWriter shifted;
    shifted.write_bits(0x5, 3);
    shifted.write_bit_range(data.data(), 0, bits);
    shifted.write_bits(0x2a, 6);
    const auto at = read_move_packed(shifted.data(), shifted.byte_size(), 3, 3 + bits);
    CHECK(at && at->new_move.time_stamp == 3.0f && at->new_move.location_cm().x == 10.0, "a parameter at a bit offset inside a bunch reads, bounded by `end`");
    CHECK(!read_move_packed(shifted.data(), shifted.byte_size(), 3, 3 + bits + 6), "(but not when `end` runs past it: leftover bits)");
}

// ---- the buffer's bounds and hostile parameters --------------------------------------------------------------------------------

static void test_buffer_bounds() {
    std::printf("the reader stays inside its buffer\n");
    MovePacked p;
    p.new_move = walking_move(3.0f, 10, 20, 30);
    size_t bits = 0;
    const auto data = written(p, &bits);
    std::string why;
    CHECK(read_move_packed(data.data(), data.size(), 0, bits).has_value(), "(the move reads from a buffer of its own size)");
    CHECK(!read_move_packed(data.data(), data.size(), 0, data.size() * 8 + 1, &why) && why.find("buffer") != std::string::npos,
          "an `end` one bit past the buffer: refused, with a reason, before anything is read");
    CHECK(!read_move_packed(data.data(), data.size(), 0, data.size() * 8 + 4096), "an `end` 4,096 bits past the buffer: refused");
    CHECK(!read_move_packed(data.data(), data.size() - 1, 0, bits), "a buffer one byte short of the parameter: refused, not read");
    CHECK(!read_move_packed(data.data(), 0, 0, bits), "a buffer of no bytes: refused");
    CHECK(!read_move_packed(data.data(), data.size(), 0, ~size_t(0)), "an `end` of size_t's maximum: refused (no overflow in the byte count)");
}

namespace fuzz {

using Bits = std::vector<uint8_t>;   // one byte per bit

static uint64_t g_state = 0x9e3779b97f4a7c15ull;   // xorshift64: the same inputs on every run
static uint64_t rnd() {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 7;
    g_state ^= g_state << 17;
    return g_state;
}
static uint64_t below(uint64_t n) { return n ? rnd() % n : 0; }

static Bits unpack(const uint8_t* d, size_t bits) {
    Bits b(bits);
    for (size_t i = 0; i < bits; ++i) b[i] = (d[i >> 3] >> (i & 7)) & 1;
    return b;
}
static Bits unpack(const BunchWriter& w) { return unpack(w.data(), w.bit_pos()); }
static Bits framed(const Bits& payload) {   // [1][sip(NumBits)][payload]
    BunchWriter w;
    w.write_bit(1);
    w.write_sip(payload.size());
    for (uint8_t b : payload) w.write_bit(b);
    return unpack(w);
}
/// The payload of a well-framed parameter (empty when the header does not read).
static Bits payload_of(const Bits& p) {
    if (p.empty()) return {};
    size_t i = 1;
    for (int k = 0; k < 10 && i + 8 <= p.size(); ++k) {
        uint8_t byte = 0;
        for (int j = 0; j < 8; ++j) byte |= static_cast<uint8_t>(p[i + j] << j);
        i += 8;
        if (!(byte & 1)) return Bits(p.begin() + static_cast<std::ptrdiff_t>(i), p.end());
    }
    return {};
}

struct FuzzStats {
    size_t calls = 0, read = 0, unstable = 0, nonfinite = 0, with_old = 0, with_base = 0, longest_bone = 0;
};

/// One read from a heap buffer of exactly the bytes the parameter needs (an address sanitizer build reports a read past it),
/// at a bit offset `prefix`; whatever was read is written back and must read back to the same bits.
static void call(const Bits& param, size_t prefix, FuzzStats& st) {
    const size_t end = prefix + param.size();
    const size_t nbytes = (end + 7) / 8;
    std::unique_ptr<uint8_t[]> buf(new uint8_t[nbytes ? nbytes : 1]);
    for (size_t i = 0; i < nbytes; ++i) buf[i] = static_cast<uint8_t>(rnd());   // junk everywhere, also past `end` in the last byte
    for (size_t i = 0; i < param.size(); ++i) {
        const size_t at = prefix + i;
        buf[at >> 3] = static_cast<uint8_t>((buf[at >> 3] & ~(1u << (at & 7))) | (param[i] << (at & 7)));
    }
    ++st.calls;
    const auto m = read_move_packed(buf.get(), nbytes, prefix, end);
    if (!m) return;
    ++st.read;
    st.with_old += m->old.has_value();
    for (const NetworkMove* n : {&m->new_move, m->pending ? &*m->pending : nullptr, m->old ? &*m->old : nullptr}) {
        if (!n) continue;
        st.with_base += n->base.has_value();
        if (n->base_bone) st.longest_bone = std::max(st.longest_bone, n->base_bone->text.size());
        const Vec3d l = n->location_cm(), a = n->acceleration_cm();
        if (!std::isfinite(l.x) || !std::isfinite(l.y) || !std::isfinite(l.z) || !std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z) ||
            !std::isfinite(n->time_stamp))
            ++st.nonfinite;
    }
    // (a parameter the writer takes comes back out in a form that reads and writes to itself: bits that are not canonical,
    // a wider quantized vector than needed or an optional byte equal to its default, settle after one rewrite)
    BunchWriter w;
    if (write_move_packed(w, *m)) {
        const Bits again = unpack(w);
        const size_t nb2 = (again.size() + 7) / 8;
        std::unique_ptr<uint8_t[]> b2(new uint8_t[nb2]());
        for (size_t i = 0; i < again.size(); ++i) b2[i >> 3] |= static_cast<uint8_t>(again[i] << (i & 7));
        const auto m2 = read_move_packed(b2.get(), nb2, 0, again.size());
        BunchWriter w3;
        if (!m2 || !write_move_packed(w3, *m2) || unpack(w3) != again) ++st.unstable;
    }
}

static Bits mutate(Bits p) {
    const int ops = 1 + static_cast<int>(below(4));
    for (int k = 0; k < ops; ++k) {
        switch (below(8)) {
            case 0:   // flip a few bits
                for (uint64_t n = 1 + below(8); n && !p.empty(); --n) p[below(p.size())] ^= 1;
                break;
            case 1:   // truncate
                if (!p.empty()) p.resize(below(p.size()));
                break;
            case 2:   // extend with random bits
                for (uint64_t n = 1 + below(200); n; --n) p.push_back(rnd() & 1);
                break;
            case 3: { // a run of ones or zeros
                if (p.empty()) break;
                const size_t a = below(p.size()), len = 1 + below(64);
                const uint8_t v = rnd() & 1;
                for (size_t i = a; i < p.size() && i < a + len; ++i) p[i] = v;
                break;
            }
            case 4: { // insert
                const size_t a = p.empty() ? 0 : below(p.size() + 1);
                Bits ins;
                for (uint64_t n = 1 + below(40); n; --n) ins.push_back(rnd() & 1);
                p.insert(p.begin() + static_cast<std::ptrdiff_t>(a), ins.begin(), ins.end());
                break;
            }
            case 5: { // delete
                if (p.empty()) break;
                const size_t a = below(p.size()), len = 1 + below(40);
                p.erase(p.begin() + static_cast<std::ptrdiff_t>(a), p.begin() + static_cast<std::ptrdiff_t>(std::min(p.size(), a + len)));
                break;
            }
            case 6:   // the container's flag bits at the end
                if (p.size() > 8) {
                    for (size_t i = 1; i <= 4; ++i) p[p.size() - i] = rnd() & 1;
                }
                break;
            default:  // 32 bits of a special float pattern
                if (p.size() > 40) {
                    static const uint32_t pats[] = {0x7fc00000u, 0x7f800000u, 0xff800000u, 0x7f7fffffu, 0x00000001u, 0x80000000u, 0xffffffffu};
                    const uint32_t v = pats[below(7)];
                    const size_t a = below(p.size() - 32);
                    for (size_t i = 0; i < 32; ++i) p[a + i] = (v >> i) & 1;
                }
                break;
        }
        if (p.size() > 5000) p.resize(5000);
    }
    return p;
}
// [CoA x C-Y-B]

static QuantVec random_quant() {
    QuantVec q;
    switch (below(5)) {
        case 0:
            q.mode = QuantVec::Mode::RawFloat;
            q.x = static_cast<uint32_t>(rnd());
            q.y = static_cast<uint32_t>(rnd());
            q.z = static_cast<uint32_t>(rnd());
            break;
        case 1:
            q.mode = QuantVec::Mode::RawDouble;
            q.x = static_cast<int64_t>(rnd());
            q.y = static_cast<int64_t>(rnd());
            q.z = static_cast<int64_t>(rnd());
            break;
        default: {
            q.mode = below(2) ? QuantVec::Mode::Scaled : QuantVec::Mode::Unscaled;
            const uint64_t bits = 1 + below(below(2) ? 63 : 28);
            auto v = [&] {
                const int64_t r = static_cast<int64_t>(rnd());
                return bits >= 63 ? (r >> 1) : (r % (int64_t(1) << (bits - 1)));
            };
            q.x = v();
            q.y = v();
            q.z = v();
        }
    }
    return q;
}

static NetworkMove random_move() {
    NetworkMove m;
    m.time_stamp = std::bit_cast<float>(static_cast<uint32_t>(rnd()));
    m.acceleration = random_quant();
    m.location = random_quant();
    m.control_rotation = {static_cast<uint16_t>(rnd() & (below(2) ? 0xFFFF : 0)), static_cast<uint16_t>(rnd()), static_cast<uint16_t>(below(2) ? rnd() : 0)};
    m.compressed_flags = static_cast<uint8_t>(below(2) ? rnd() : 0);
    if (below(3) == 0) {
        FIntrepidNetworkGUID g;
        g.ObjectId = rnd() | 2;   // not 1
        g.ServerId = static_cast<uint32_t>(rnd());
        g.Randomizer = static_cast<uint32_t>(rnd());
        m.base = g;
    }
    if (below(3) == 0) {
        if (below(2)) {
            m.base_bone = NameValue::hardcoded_name(static_cast<uint32_t>(rnd()));
        } else {
            std::string s;
            for (uint64_t n = below(below(2) ? 1100 : 20); n; --n) s.push_back(static_cast<char>(1 + below(126)));
            m.base_bone = NameValue::plain(s, static_cast<int32_t>(rnd()));
        }
    }
    m.movement_mode = static_cast<uint8_t>(below(2) ? rnd() : 1);
    m.input_bits = static_cast<uint8_t>(rnd() & 15);
    return m;
}

static Bits random_valid() {
    MovePacked p;
    p.new_move = random_move();
    if (below(2)) p.pending = random_move();
    p.dual_hybrid_root_motion = below(2);
    if (below(3) == 0) p.old = random_move();
    p.disable_combined_scoped_move = below(2);
    BunchWriter w;
    if (!write_move_packed(w, p)) return {};
    return unpack(w);
}

}  // namespace fuzz

static void test_hostile_parameters() {
    std::printf("hostile parameters (a seeded fuzz, exact-size heap buffers)\n");
    using namespace fuzz;
    FuzzStats st;
    std::vector<Bits> seeds;
    for (const auto& mv : capture_moves::kE2e) {
        const auto b = bytes_of(mv.hex);
        seeds.push_back(unpack(b.data(), mv.bits));
    }
    for (size_t i = 0; i < 400; ++i) {
        const auto& mv = capture_moves::kNative[i * 4];
        const auto b = bytes_of(mv.hex);
        seeds.push_back(unpack(b.data(), mv.bits));
    }
    for (const Bits& s : seeds) call(s, 0, st);
    const size_t seeds_read = st.read;
    constexpr size_t kIterations = 40000;
    for (size_t i = 0; i < kIterations; ++i) {
        const size_t prefix = below(4) ? 0 : below(40);
        switch (below(6)) {
            case 0:   // a mutated capture, NumBits fitted again
                call(framed(mutate(payload_of(seeds[below(seeds.size())]))), prefix, st);
                break;
            case 1: { // a mutated capture, header kept (NumBits then usually disagrees)
                Bits s = seeds[below(seeds.size())];
                if (s.size() > 10) {
                    const Bits tail = mutate(Bits(s.begin() + 10, s.end()));
                    s.resize(10);
                    s.insert(s.end(), tail.begin(), tail.end());
                }
                call(s, prefix, st);
                break;
            }
            case 2: { // random bits, NumBits right, up to past the limit
                Bits pl;
                const size_t n = below(below(2) ? 700 : 4200);
                const uint64_t bias = below(3);
                for (size_t k = 0; k < n; ++k) pl.push_back(bias == 0 ? (rnd() & 1) : bias == 1 ? (below(8) == 0) : (below(8) != 0));
                call(framed(pl), prefix, st);
                break;
            }
            case 3: { // a random valid container, maybe mutated
                Bits v = random_valid();
                if (v.empty()) break;
                if (below(3) == 0) v = framed(mutate(payload_of(v)));
                call(v, prefix, st);
                break;
            }
            case 4: { // a hostile NumBits over a small body
                BunchWriter w;
                w.write_bit(1);
                w.write_sip(below(3) == 0 ? rnd() : (below(2) ? 4096 + below(10) : below(5000)));
                for (size_t k = below(600); k; --k) w.write_bit(rnd() & 1);
                call(unpack(w), prefix, st);
                break;
            }
            default:  // all ones or all zeros, every length
                call(framed(Bits(below(4200), static_cast<uint8_t>(below(2)))), prefix, st);
                break;
        }
    }
    std::printf("    %zu reads: %zu read (%zu of the %zu captured seeds), %zu with an old move, %zu bases, longest bone name %zu, %zu non-finite numbers\n", st.calls, st.read,
                seeds_read, seeds.size(), st.with_old, st.with_base, st.longest_bone, st.nonfinite);
    CHECK(seeds_read == seeds.size(), "(every captured seed reads)");
    CHECK(st.read > seeds.size() + 1000, "(the generators reach well past the seeds: thousands of other parameters read)");
    CHECK(st.unstable == 0, "whatever reads writes back to bits that read and write to themselves");
    CHECK(st.nonfinite > 0, "(the fuzz reaches non-finite numbers: the reader passes them on, the checker refuses them)");
    // (reaching this line is the other check: no crash and no hang; a read past a buffer is an address sanitizer report)
}
// [CoA :: C.Y.B]

int main() {
    test_captures();
    test_first_move();
    test_made_moves();
    test_refusals();
    test_buffer_bounds();
    test_hostile_parameters();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
