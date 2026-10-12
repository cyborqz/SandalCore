// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_npc.cpp
//
//  plan E1, stage 1: the other actors' schemas against what the retail server sent (capture_npc.h, tools/capture/gen_npc.py).
//
//    NPC opens          every NPC open of capture 957 (330: the 323 the research counted and seven whose subobjects the capture never
//                       named): the actor block with ANPCCharacter (ReplicatedMovement, Owner, Controller, the timestamp,
//                       ReplicatedMovementMode and CurrentMovementInput through the new FPlayerMovementInput codec), UAlignmentComponent,
//                       UNPCSecondaryInfo (the InteractInfo subobject), UNPCInfo, UNPCCombatInfo, UAoCAbilityComponent and
//                       UAoCStatsComponent, and for humanoids UNPCAppearanceComponent: each block reads to its last bit (a RepLayout block,
//                       then the field loop: fast arrays and RPC fields) and writes back to the same bits
//    NPC updates        the 2,620 updates of those channels (ReplicatedMovement and its timestamp, CurrentMovementInput alone, the stats'
//                       fast arrays, the shared multicast RPC, whose payload has no codec yet: its field header and bit count only)
//    gatherables        the 220 AGatherableActor opens of the same capture, each with its UInteractableComponent
//    vendors            the 9 vendor NPC opens of capture 716: UInteractableComponent with InteractVisibleStateMap items
//    FPlayerMovementInput  the four bits as a value: every direction combination reads and writes, an out-of-range value is refused
//    hostile            a seeded fuzz of the captured blocks (bit flips, cuts, noise tails, each from a heap buffer of exactly the bytes it
//                       needs): nothing crashes, and whatever reads writes back to bits that read equal
// ============================================================================
#include "capture_npc.h"
#include "test_support.h"

#include "protocol/repl/fast_array.h"
#include "protocol/repl/rep_block.h"
#include "protocol/schema/generated/index.h"
#include "protocol/wire/packet_reader.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using namespace tsupport;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
using emit::BunchWriter;

/// The client's stat table is not on the wire: the captured pawn's four extended stats have Records 0x5429e8c0xxxxxxxx and 0x5429e8c4xxxxxxxx
/// (test_repl_fast_array.cpp); an NPC has none of them.
static CodecOptions options(bool short_rotation, bool preset_known) {
    CodecOptions o;
    o.rep_movement_short_rotation = short_rotation;
    o.stat_int32_extended = [](int64_t record) {
        const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(record) >> 32);
        return hi == 0x5429e8c0u || hi == 0x5429e8c4u;
    };
    if (preset_known) o.customization_preset_known = [](uint64_t) { return true; };
    return o;
}
// C-Y-B captures, CoA code

/// One content block as read: its RepLayout block, its fast arrays, and the RPC fields it carries (read only as far as their header).
struct Read {
    std::optional<RepState>        state;
    std::vector<FastArrayField>    arrays;
    std::vector<std::pair<uint32_t, size_t>> rpcs;   // field index, payload bits
    CodecOptions                   opts;
};

static bool read_block(const schema::ClassSchema& s, const uint8_t* data, size_t bits, bool rep, Read& out) {
    for (const bool short_rot : {false, true}) {
        for (const bool known : {false, true}) {
            out = Read{};
            out.opts = options(short_rot, known);
            size_t pos = 0;
            if (rep) {
                out.state = read_rep_block(s, data, bits, pos, out.opts);
                if (!out.state) continue;
            }
            bool ok = true;
            while (pos < bits && ok) {
                const auto h = read_field_header(s, data, bits, pos);
                if (!h) { ok = false; break; }
                const size_t end = pos + h->payload_bits;
                const schema::NetField* f = s.field(h->field);
                if (!f) { ok = false; break; }
                if (f->kind == schema::FieldKind::Function) {
                    out.rpcs.push_back({h->field, h->payload_bits});
                    pos = end;
                    continue;
                }
                const auto layout = fast_array_layout(s, h->field);
                if (!layout) { ok = false; break; }
                size_t p = pos;
                auto delta = read_fast_array_delta(*layout, data, end, p, out.opts);
                if (!delta || p != end) { ok = false; break; }
                out.arrays.push_back({h->field, std::move(*delta)});
                pos = end;
            }
            if (ok && pos == bits) return true;
        }
    }
    return false;
}

/// The block written again from what was read: a RepLayout block, then the fast arrays' fields.  nullopt for a block with an RPC field (written
/// bit for bit from the capture by nothing here).
static std::optional<Bits> write_block(const schema::ClassSchema& s, const Read& r, bool rep) {
    if (!r.rpcs.empty()) return std::nullopt;
    BunchWriter w;
    bool failed = false;
    if (rep) {
        RepShadow shadow;
        write_rep_block(s, *r.state, shadow, w, r.opts, &failed);
        if (failed) return std::nullopt;
    }
    FastArrays arrays;
    for (const FastArrayField& f : r.arrays) {
        FastArrayState* st = arrays.get(s, f.field);
        if (!st || !apply_fast_array_delta(*st, f.delta)) return std::nullopt;
    }
    FastArrayShadows shadows;
    write_fast_array_fields(s, arrays, shadows, w, r.opts, &failed);
    if (failed) return std::nullopt;
    return Bits(w);
}

/// The stat the block's handle 67 carries.
static const PlayerMovementInput* movement_input(const Read& r) {
    if (!r.state) return nullptr;
    const Value* v = r.state->get(67);
    return v ? v->get_if<PlayerMovementInput>() : nullptr;
}

struct Tally {
    size_t blocks = 0, read = 0, rewritten = 0, same = 0, rpc_blocks = 0, rpc_fields = 0, bits = 0;
    size_t with_input = 0;
    std::map<std::string, size_t> by_class;
    std::vector<std::string> failures;
};

static void check_block(const capture_npc::Block& b, Tally& t, const char* where) {
    const schema::ClassSchema* s = gen::find_class_schema(b.cls);
    const capture_npc::Payload& p = capture_npc::kPayloads[b.payload];
    ++t.blocks;
    ++t.by_class[b.cls];
    t.bits += p.bits;
    if (!s) { t.failures.push_back(std::string(where) + ": no schema " + b.cls); return; }
    const Bits want = bits_of_hex(p.hex, p.bits);
    Read r;
    if (!with_exact(want, [&](const uint8_t* d, size_t, size_t bits) { return read_block(*s, d, bits, b.rep, r); })) {
        t.failures.push_back(std::string(where) + ": " + b.cls + " (" + std::to_string(p.bits) + " bits) does not read to its last bit");
        return;
    }
    ++t.read;
    if (movement_input(r)) ++t.with_input;
    if (!r.rpcs.empty()) {
        ++t.rpc_blocks;
        t.rpc_fields += r.rpcs.size();
        return;
    }
    const auto again = write_block(*s, r, b.rep);
    if (!again) { t.failures.push_back(std::string(where) + ": " + b.cls + " does not write"); return; }
    ++t.rewritten;
    if (*again == want) ++t.same;
    else t.failures.push_back(std::string(where) + ": " + b.cls + " (" + std::to_string(p.bits) + " bits) writes " + std::to_string(again->size()) + " bits that differ");
}

static void report(const Tally& t, const char* what) {
    std::printf("    %s: %zu blocks, %zu bits, %zu read, %zu rewritten, %zu identical, %zu blocks with %zu RPC field(s)\n", what, t.blocks, t.bits, t.read,
                t.rewritten, t.same, t.rpc_blocks, t.rpc_fields);
    for (size_t i = 0; i < t.failures.size() && i < 8; ++i) std::printf("      %s\n", t.failures[i].c_str());
}

// ---- FPlayerMovementInput ---------------------------------------------------------------------------------------------------

static void test_movement_input() {
    std::printf("FPlayerMovementInput\n");
    const schema::ClassSchema& npc = schema_named("ANPCCharacter");
    const schema::RepCommand* cmd = npc.command("CurrentMovementInput");
    bool all = cmd != nullptr;
    for (uint8_t bits = 0; all && bits < 16; ++bits) {
        BunchWriter w;
        all = all && write_struct("FPlayerMovementInput", Value(PlayerMovementInput{bits}), w) && w.bit_pos() == 4;
        const Bits b(w);
        const auto back = with_exact(b, [&](const uint8_t* d, size_t bytes, size_t n) {
            wire::PacketReader r(d, bytes, n);
            return read_struct("FPlayerMovementInput", r);
        });
        all = all && back && back->get_if<PlayerMovementInput>() && back->get_if<PlayerMovementInput>()->bits == bits;
    }
    CHECK(all, "all sixteen direction combinations write four bits and read back");
    BunchWriter bad;
    CHECK(!write_struct("FPlayerMovementInput", Value(PlayerMovementInput{16}), bad), "a value over four bits is refused");
    CHECK(!write_struct("FPlayerMovementInput", Value::u64(1), bad), "a value of another kind is refused");
    CHECK(zero_struct("FPlayerMovementInput") && zero_struct("FPlayerMovementInput")->get_if<PlayerMovementInput>() &&
              zero_struct("FPlayerMovementInput")->get_if<PlayerMovementInput>()->bits == 0,
          "its zero value is no direction held");
    RepState st;
    CHECK(st.set(npc, "CurrentMovementInput", Value(PlayerMovementInput{PlayerMovementInput::kForward})) && !st.set(npc, "CurrentMovementInput", Value::u64(1)),
          "an NPC's CurrentMovementInput takes it (and nothing else)");
    CHECK(st.erase(67) && !st.erase(67) && st.get(67) == nullptr, "RepState::erase forgets a value once");
}

// ---- the captured opens and updates -----------------------------------------------------------------------------------------
// CoA and C:Y:B

static void test_opens() {
    using namespace capture_npc;
    std::printf("opens and updates of capture 957 and 716\n");
    Tally npc_open, npc_up, gat_open, gat_up, vendor_open;
    size_t npc_opens = 0, gat_opens = 0, vendor_opens = 0, updates_npc = 0, updates_gat = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const Open& o : kOpens) {
        const bool gat = std::string(o.category) == "gatherable";
        const bool vendor = o.capture == 716;
        Tally& open_tally = vendor ? vendor_open : gat ? gat_open : npc_open;
        (vendor ? vendor_opens : gat ? gat_opens : npc_opens)++;
        std::string where = "capture " + std::to_string(o.capture) + " ch " + std::to_string(o.channel) + " pkt " + std::to_string(o.packet);
        for (uint32_t i = 0; i < o.blocks; ++i) check_block(kBlocks[o.first_block + i], open_tally, where.c_str());
        for (uint32_t u = 0; u < o.updates; ++u) {
            const Update& up = kUpdates[o.first_update + u];
            (gat ? updates_gat : updates_npc)++;
            for (uint32_t i = 0; i < up.blocks; ++i) check_block(kUpdateBlocks[up.first_block + i], gat ? gat_up : npc_up, (where + " update " + std::to_string(up.packet)).c_str());
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    report(npc_open, "NPC opens");
    report(npc_up, "NPC updates");
    report(gat_open, "gatherable opens");
    report(gat_up, "gatherable updates");
    report(vendor_open, "vendor NPC opens");
    std::printf("    (%.2f s)\n", secs);
    for (const auto& [cls, n] : npc_open.by_class) std::printf("    NPC opens: %zu x %s\n", n, cls.c_str());
    std::printf("    NPC opens with CurrentMovementInput: %zu; NPC update blocks with it: %zu\n", npc_open.with_input, npc_up.with_input);
    CHECK(npc_opens == 330 && updates_npc == 2620, "capture 957: 330 NPC opens and 2,620 updates");
    CHECK(npc_open.failures.empty() && npc_open.read == npc_open.blocks && npc_open.blocks == 330 + 323 * 6,
          "every block of every NPC open reads to its last bit (323 opens of the actor block and six subobjects: the 323/323 of the research; 7 opens that carry the actor block alone)");
    CHECK(npc_open.same == npc_open.rewritten && npc_open.rewritten == npc_open.blocks,
          "and writes back to the same bits (the shape ours would send is the retail one)");
    CHECK(npc_open.with_input == 230, "FPlayerMovementInput ends the actor block of 230 NPC opens (CurrentMovementInput, handle 67; the research's 69 %)");
    CHECK(npc_open.by_class["UNPCInfo"] == 323 && npc_open.by_class["UNPCCombatInfo"] == 323 && npc_open.by_class["UNPCSecondaryInfo"] == 323 &&
              npc_open.by_class["UAoCStatsComponent"] == 323 && npc_open.by_class["UAoCAbilityComponent"] == 323 && npc_open.by_class["UAlignmentComponent"] == 323 &&
              npc_open.by_class["ANPCCharacter"] == 330,
          "the standing classes appear once per NPC with subobjects");
    CHECK(npc_up.failures.empty() && npc_up.read == npc_up.blocks && npc_up.blocks >= 2620,
          "every block of every NPC update reads to its last bit");
    CHECK(npc_up.same == npc_up.rewritten && npc_up.rewritten + npc_up.rpc_blocks == npc_up.blocks,
          "the property updates (ReplicatedMovement + timestamp, CurrentMovementInput, stats) write back to the same bits; the others hold the shared multicast RPC");
    CHECK(npc_up.rpc_fields > 0 && npc_up.with_input > 0, "the updates include the RPC and CurrentMovementInput-only updates");
    CHECK(gat_opens == 220 && gat_open.failures.empty() && gat_open.read == gat_open.blocks && gat_open.same == gat_open.rewritten && gat_open.by_class["AGatherableActor"] == 220 &&
              gat_open.by_class["UInteractableComponent"] >= 200,
          "capture 957: 220 gatherable opens, AGatherableActor and its UInteractableComponent, read and written back");
    CHECK(gat_up.failures.empty() && gat_up.read == gat_up.blocks, "gatherable updates read");
    CHECK(vendor_opens == 9 && vendor_open.failures.empty() && vendor_open.read == vendor_open.blocks && vendor_open.same == vendor_open.rewritten &&
              vendor_open.by_class["UInteractableComponent"] == 9 && vendor_open.by_class["UNPCAppearanceComponent"] >= 7,
          "capture 716: 9 vendor NPC opens with their UInteractableComponent and appearance, read and written back");
}

// ---- hostile input ---------------------------------------------------------------------------------------------------------

static void test_fuzz() {
    using namespace capture_npc;
    std::printf("hostile input\n");
    Rng rng(0xE1A57A6Eull);
    size_t tried = 0, read = 0, stable = 0, crashes_avoided = 0;
    // one block of each class and size is plenty: every payload of the first 40 opens, 6 mutations each
    for (size_t oi = 0; oi < 40 && oi < sizeof(kOpens) / sizeof(kOpens[0]); ++oi) {
        const Open& o = kOpens[oi * 8 % (sizeof(kOpens) / sizeof(kOpens[0]))];
        for (uint32_t i = 0; i < o.blocks; ++i) {
            const Block& b = kBlocks[o.first_block + i];
            const schema::ClassSchema* s = gen::find_class_schema(b.cls);
            const Payload& p = kPayloads[b.payload];
            if (!s || p.bits == 0) continue;
            for (int m = 0; m < 6; ++m) {
                Bits bits = bits_of_hex(p.hex, p.bits);
                switch (m) {
                    case 0: { const size_t at = rng.below(bits.size()); bits.b[at] = !bits.b[at]; break; }   // a bit flip
                    case 1: bits.erase(bits.size() / 2, 1 + rng.below(bits.size() / 2)); break;          // a cut
                    case 2: bits.append(rng.next(), 61); break;                                          // a noisy tail
                    case 3: bits.set(rng.below(bits.size() > 32 ? bits.size() - 32 : 1), 0xffffffffu, 32 < static_cast<int>(bits.size()) ? 32 : 1); break;   // a special word
                    case 4: bits.set(rng.below(bits.size() > 16 ? bits.size() - 16 : 1), 0, 16 < static_cast<int>(bits.size()) ? 16 : 1); break;
                    default: bits = bits.slice(0, rng.below(bits.size() + 1)); break;                    // truncated
                }
                ++tried;
                Read r;
                const bool ok = with_exact(bits, [&](const uint8_t* d, size_t, size_t n) { return n > 0 && read_block(*s, d, n, b.rep, r); });
                ++crashes_avoided;
                if (!ok) continue;
                ++read;
                const auto again = write_block(*s, r, b.rep);
                if (!again) { ++stable; continue; }   // an RPC block, or a value the writer refuses: not written
                Read back;
                const bool same = with_exact(*again, [&](const uint8_t* d, size_t, size_t n) { return read_block(*s, d, n, b.rep, back); }) &&
                                  back.state == r.state && back.arrays.size() == r.arrays.size();
                if (same) ++stable;
            }
        }
    }
    std::printf("    %zu mutated blocks read (%zu), %zu of them write back to bits that read equal\n", tried, read, stable);
    CHECK(tried > 1000 && crashes_avoided == tried, "the fuzz ran on exact-size heap buffers without a crash");
    CHECK(stable == read, "whatever reads writes back to bits that read equal");
}

int main() {
    test_movement_input();
    test_opens();
    test_fuzz();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
