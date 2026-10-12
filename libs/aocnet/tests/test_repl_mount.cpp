// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_mount.cpp
//
//  The mounts milestone, stage 1: the mount actors and the callouts of the retail captures (capture_mount.h, tools/capture/gen_mount.py) against the
//  generated schemas and the new FLocalizedMessage codec.
//
//    donors      the four mount opens (911 a ground mount, 896 a second ground one, 898 an aquatic one, 871 a flying one): the actor block with
//                ANPCCharacter and the eight subobjects (UAlignmentComponent, UNPCSecondaryInfo, UNPCInfo, UNPCCombatInfo, UAoCAbilityComponent,
//                UAoCStatsComponent, UMountComponent and UInteractableComponent) each read to its last bit and write back to the same bits; the NetGUID
//                pattern (A+4 ... A+14, the MountComponent A+18, the InteractableComponent A+20), the export names the three kinds of mount component
//                resolve through, and what each block holds (the summoning flag, the owner, the stats at the owner's level)
//    callouts    nine PlayerController blocks whose CalloutQueueReplication holds one item: the refusal "Cannot summon mount here." (captures 876 and
//                870: the plain Message, an empty FLocalizedMessage) and seven messages that fill FLocalizedMessage; read, the item's values checked,
//                written back to the same bits
//    codec       FLocalizedMessage as a value: the empty message is 96 bits, "Economy" / "UnequipFailCorruption" 336; what it refuses
//    hostile     a seeded fuzz of the captured blocks (bit flips, cuts, noise tails, each from a heap buffer of exactly the bytes it needs)
// ============================================================================
#include "capture_mount.h"
#include "test_support.h"

#include "protocol/repl/callout_codec.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/fast_array.h"
#include "protocol/repl/rep_block.h"
#include "protocol/schema/generated/index.h"
#include "protocol/wire/packet_reader.h"

#include <bit>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using namespace tsupport;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
using emit::BunchWriter;
// ~ C-Y-B :: CoA ~

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

struct Read {
    std::optional<RepState>        state;
    std::vector<FastArrayField>    arrays;
    std::vector<std::pair<uint32_t, size_t>> rpcs;
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

/// `prime_base`: the connection's shadow holds the array at the delta's BaseReplicationKey (an update the retail server sent against what the client had:
/// a callout is added to an array that already held its earlier ones), so the write is the same delta.
static std::optional<Bits> write_block(const schema::ClassSchema& s, const Read& r, bool rep, bool prime_base = false) {
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
    if (prime_base)
        for (const FastArrayField& f : r.arrays) shadows.by_field[f.field].array_key = f.delta.base_key;
    write_fast_array_fields(s, arrays, shadows, w, r.opts, &failed);
    if (failed) return std::nullopt;
    return Bits(w);
}

static const Value* handle(const Read& r, uint32_t h) { return r.state ? r.state->get(h) : nullptr; }

static bool guid_set(const Read& r, uint32_t first) {
    for (uint32_t i = 0; i < 4; ++i) {
        const Value* v = handle(r, first + i);
        if (v && v->get_if<int64_t>() && *v->get_if<int64_t>() != 0) return true;
        if (v && v->get_if<uint64_t>() && *v->get_if<uint64_t>() != 0) return true;
    }
    return false;
}

static uint64_t int_of(const Value* v) {
    if (!v) return 0;
    if (const int64_t* i = v->get_if<int64_t>()) return static_cast<uint64_t>(*i);
    if (const uint64_t* u = v->get_if<uint64_t>()) return *u;
    return 0;
}

/// The value of a stat in a stats block's fast array (the float bits of FStatInt32InstRep.Value); NaN when the block has no such record.
static double stat_value(const Read& r, int64_t record) {
    for (const FastArrayField& f : r.arrays) {
        for (const auto& c : f.delta.changed) {
            for (const auto& [h, v] : c.values.values) {
                const StatInt32Inst* si = v.get_if<StatInt32Inst>();
                if (si && si->record == record) return std::bit_cast<float>(si->value);
            }
        }
    }
    return std::nan("");
}

constexpr int64_t kHealth = 108986352686712ll, kMaxHealth = 108986356618873ll, kMoveSpeedMult = 109228544644729ll;

// ---- the donors -------------------------------------------------------------------------------------------------------------

static void test_donors() {
    using namespace capture_mount;
    std::printf("the four mount opens (captures 911, 896, 898, 871)\n");
    const char* classes[9] = {"ANPCCharacter", "UAlignmentComponent", "UNPCSecondaryInfo", "UNPCInfo", "UNPCCombatInfo", "UAoCAbilityComponent",
                              "UAoCStatsComponent", "UMountComponent", "UInteractableComponent"};
    const char* names[9] = {"", "AlignmentComponent", "InteractInfo", "BaseCharacterInfo", "CombatInfo", "AbilityComponent", "StatsComponent", "", "InteractableComponent"};
    const int offsets[9] = {0, 4, 6, 8, 10, 12, 14, 18, 20};   // the NetGUID pattern: the actor's id A + these (A + 2 is the CollisionCylinder, A + 16 is never exported)
    size_t read = 0, same = 0, total = 0;
    std::set<std::string> kinds;
    for (const Donor& d : kDonors) {
        kinds.insert(d.kind);
        const std::string where = std::string(d.kind) + " capture " + std::to_string(d.capture);
        bool shape = d.blocks == 9;
        std::map<std::string, Read> reads;
        for (uint32_t i = 0; i < d.blocks && shape; ++i) {
            const Block& b = kBlocks[d.first_block + i];
            const Payload& p = kPayloads[b.payload];
            ++total;
            const schema::ClassSchema* s = gen::find_class_schema(b.cls);
            Read r;
            const Bits want = bits_of_hex(p.hex, p.bits);
            const bool ok = s && with_exact(want, [&](const uint8_t* dd, size_t, size_t bits) { return read_block(*s, dd, bits, b.rep, r); });
            if (!ok) { std::printf("    %s: block %u (%s, %u bits) does not read to its last bit\n", where.c_str(), i, b.cls, p.bits); shape = false; break; }
            ++read;
            const auto again = write_block(*s, r, b.rep);
            if (again && *again == want) ++same;
            else std::printf("    %s: block %u (%s) does not write back to the same bits\n", where.c_str(), i, b.cls);
            std::string mount_name = b.name;
            const bool mount_comp = std::string(b.cls) == "UMountComponent";
            shape = shape && std::string(b.cls) == classes[i] && (mount_comp ? !mount_name.empty() : mount_name == names[i]) && (i == 0) == (b.is_actor == 1) &&
                    (i == 0 ? b.object_id == 0 : b.object_id == d.actor_id + static_cast<uint64_t>(offsets[i]));
            reads[b.cls] = std::move(r);
        }
        CHECK(shape, (where + ": the actor block and eight subobjects, in the retail order, at the NetGUID offsets A+4 ... A+14, A+18 (MountComponent) and A+20 (InteractableComponent)").c_str());
        if (!shape) continue;
        // the three export names of the mount component resolve to the generated class
        const Block& mc = kBlocks[d.first_block + 7];
        const std::string want_name = std::string(d.kind) == "ground" ? "GroundMountComponent" : std::string(d.kind) == "aquatic" ? "AquaticMountComponent" : "FlyingMountComponent";
        CHECK(want_name == mc.name && gen::find_component_schema(mc.name) == gen::find_class_schema("UMountComponent") && mc.checksum != 0,
              (where + ": its MountComponent is the export " + want_name + " (a UMountComponent, with a checksum)").c_str());
        for (uint32_t i = 1; i < 9; ++i) {
            const Block& b = kBlocks[d.first_block + i];
            if (std::string(b.cls) == "UMountComponent") continue;
            // an NPC's BaseCharacterInfo and CombatInfo are its own classes (UNPCInfo, UNPCCombatInfo), found by class name, not by the pawn's component table
            const bool npc_variant = std::string(b.name) == "BaseCharacterInfo" || std::string(b.name) == "CombatInfo";
            if (std::string(names[i]) != b.name || (!npc_variant && gen::find_component_schema(b.name) != gen::find_class_schema(b.cls))) { shape = false; }
        }
        CHECK(shape, (where + ": every stably named subobject resolves by its export name to the class that reads it").c_str());
        // what the blocks hold
        const Read& mount = reads["UMountComponent"];
        const Value* summoning = handle(mount, 10);
        const Value* dissolve = handle(mount, 11);
        CHECK(summoning && summoning->get_if<bool>() && *summoning->get_if<bool>() && guid_set(mount, 5) && dissolve && dissolve->get_if<float>() &&
                  *dissolve->get_if<float>() > 0 && *dissolve->get_if<float>() < 0.1f && !handle(mount, 9),
              (where + ": UMountComponent: bIsSummoning, the item's guid (ItemInstanceGuid) and a DissolveTimer under 0.1 s; not desummoning").c_str());
        const Read& owner = reads["UNPCSecondaryInfo"];
        const Read& info = reads["UNPCInfo"];
        CHECK(guid_set(owner, 22) && guid_set(info, 3) && handle(info, 9) && handle(info, 9)->get_if<std::string>() && !handle(info, 9)->get_if<std::string>()->empty(),
              (where + ": InteractInfo names the owner (CreatureOwnerGuid), BaseCharacterInfo the mount's own CharacterGuid and the owner's name").c_str());
        const Read& stats = reads["UAoCStatsComponent"];
        const double max_health = stat_value(stats, kMaxHealth), health = stat_value(stats, kHealth), move = stat_value(stats, kMoveSpeedMult);
        CHECK(max_health > 0 && health == max_health && move >= 1.0, (where + ": the stats: Health at its MaxHealth (" + std::to_string(max_health) + "), MoveSpeedMult >= 1").c_str());
        if (d.capture == 911) CHECK(max_health == 3572.0, "capture 911: a level 20 owner's terror bird has MaxHealth 3572 (Mount_Base_HP at level 20)");
        if (d.capture == 871) CHECK(max_health == 24375.0, "capture 871: a level 25 owner's Dawnbreaker has MaxHealth 24375 (4875 x 5)");
        if (d.capture == 911 || d.capture == 871 || d.capture == 898) {
            const bool known = std::string(d.archetype).find("/Game/GameBlueprints/Mounts/Mount_") == 0 && std::string(d.archetype).find(".Default__Mount_") != std::string::npos &&
                               d.archetype_id != 0 && d.archetype_checksum == 47039926u;
            CHECK(known, (where + ": the archetype is the Blueprint's Default__ object under /Game/GameBlueprints/Mounts/ (checksum 47039926, the class default objects' of every mount)").c_str());
        } else {
            CHECK(std::string(d.archetype).empty() && d.archetype_id == 0, (where + ": the capture never exported the raptor's archetype: blocks only, no donor for the server").c_str());
        }
    }
    CHECK(total == 36 && read == total && same == total, "36 blocks (4 opens x 9) read to their last bit and write back to the same bits");
    CHECK(kinds.size() == 3 && kinds.count("ground") && kinds.count("aquatic") && kinds.count("flying"), "a ground, an aquatic and a flying mount");
}
// C.Y.B and CoA

// ---- the callouts -----------------------------------------------------------------------------------------------------------

static void test_callouts() {
    using namespace capture_mount;
    std::printf("the PlayerController's callouts (captures 876, 870 and seven messages with a FLocalizedMessage)\n");
    const schema::ClassSchema& pc = schema_named("AoCPlayerControllerBP_C");
    const schema::NetField* fld = pc.field("CalloutQueueReplication");
    size_t read = 0, same = 0, localized = 0, refusals = 0;
    for (const Callout& c : kCallouts) {
        const Payload& p = kPayloads[c.payload];
        const std::string where = "capture " + std::to_string(c.capture) + " packet " + std::to_string(c.packet);
        const Bits want = bits_of_hex(p.hex, p.bits);
        Read r;
        const bool ok = with_exact(want, [&](const uint8_t* d, size_t, size_t bits) { return read_block(pc, d, bits, c.rep, r); });
        if (!ok || r.arrays.size() != 1 || !fld || r.arrays[0].field != fld->index) {
            CHECK(false, (where + ": the block reads to its last bit as one CalloutQueueReplication update").c_str());
            continue;
        }
        ++read;
        const FastArrayDelta& delta = r.arrays[0].delta;
        const auto layout = fast_array_layout(pc, fld->index);
        const schema::RepCommand* message_cmd = nullptr;
        uint16_t message_h = 0, local_h = 0;
        for (size_t i = 0; layout && i < layout->element.size(); ++i) {
            const std::string path = layout->element[i].path;
            if (path == "Items.Value.Message") { message_cmd = &layout->element[i]; message_h = static_cast<uint16_t>(i + 1); }
            if (path == "Items.Value.LocalizedMessage") local_h = static_cast<uint16_t>(i + 1);
        }
        bool shape = delta.changed.size() == 1 && delta.deleted.empty() && message_cmd && local_h;
        std::string message, table, key;
        bool has_lm = false;
        if (shape) {
            const ElementState& item = delta.changed[0].values;
            const auto m = item.values.find(message_h);
            const auto l = item.values.find(local_h);
            if (m != item.values.end() && m->second.get_if<std::string>()) message = *m->second.get_if<std::string>();
            if (l != item.values.end() && l->second.get_if<LocalizedMessage>()) {
                has_lm = true;
                table = l->second.get_if<LocalizedMessage>()->string_table_id;
                key = l->second.get_if<LocalizedMessage>()->key;
            }
        }
        CHECK(shape && has_lm && message == c.message && table == c.table && key == c.key,
              (where + ": one item: Message '" + message + "', LocalizedMessage " + (table.empty() && key.empty() ? "empty" : "'" + table + "' / '" + key + "'")).c_str());
        const auto again = write_block(pc, r, c.rep, true);
        if (again && *again == want) ++same;
        else {
            size_t at = 0;
            if (again) while (at < again->size() && at < want.size() && again->b[at] == want.b[at]) ++at;
            std::printf("    %s: the block does not write back to the same bits (%zu written, %zu read; the first difference at bit %zu)\n", where.c_str(), again ? again->size() : 0,
                        want.size(), at);
            if (again && again->size() == want.size()) {
                std::string diffs;
                for (size_t i = 0; i < want.size() && diffs.size() < 120; ++i)
                    if (again->b[i] != want.b[i]) diffs += std::to_string(i) + (want.b[i] ? "(1->0) " : "(0->1) ");
                std::printf("      differing bits: %s\n", diffs.c_str());
            }
        }
        if (!table.empty()) ++localized;
        if (message == "Cannot summon mount here.") ++refusals;
    }
    CHECK(read == 9 && same == 9, "nine callout blocks read to their last bit and write back to the same bits");
    CHECK(refusals == 2 && localized == 7, "two are the refused summon's (the plain Message, an empty FLocalizedMessage), seven fill FLocalizedMessage");
}

// ---- the codec --------------------------------------------------------------------------------------------------------------

static std::optional<LocalizedMessage> read_msg(const Bits& bits) {
    return with_exact(bits, [&](const uint8_t* d, size_t bytes, size_t n) -> std::optional<LocalizedMessage> {
        wire::PacketReader r(d, bytes, n);
        auto m = read_localized_message(r);
        if (!m || r.pos() != n) return std::nullopt;
        return m;
    });
}

static void test_codec() {
    std::printf("FLocalizedMessage as a value\n");
    BunchWriter w;
    CHECK(write_localized_message(w, LocalizedMessage{}) && w.bit_pos() == 96, "the empty message is 96 bits (two empty FStrings and an empty argument list)");
    const Bits empty(w);
    CHECK(empty.get(0, 32) == 0 && empty.get(32, 32) == 0 && empty.get(64, 32) == 0, "...all zero, as in captures 876 and 870");
    const auto back = read_msg(empty);
    CHECK(back && *back == LocalizedMessage{}, "and reads back");
    LocalizedMessage econ{"Economy", "UnequipFailCorruption"};
    BunchWriter w2;
    CHECK(write_localized_message(w2, econ) && w2.bit_pos() == 336, "Economy / UnequipFailCorruption is 336 bits (as in capture 1017)");
    const auto back2 = read_msg(Bits(w2));
    CHECK(back2 && *back2 == econ, "and reads back");
    BunchWriter w3;
    CHECK(write_localized_message(w3, LocalizedMessage{"NCS", "Event_Toast_Failed"}) && w3.bit_pos() == 280, "NCS / Event_Toast_Failed is 280 bits (capture 1018)");
    // through the struct registry: the generated schema's command names it
    const schema::ClassSchema& pc = schema_named("AoCPlayerControllerBP_C");
    const schema::RepCommand* cmd = nullptr;
    for (const schema::RepCommand& c : pc.element_commands)
        if (std::string(c.path) == "Items.Value.LocalizedMessage") cmd = &c;
    CHECK(cmd && cmd->type == schema::PropType::NetSerializeStruct && std::string(cmd->struct_name) == "FLocalizedMessage" && is_known_struct("FLocalizedMessage"),
          "the callout's LocalizedMessage command is a NetSerialize struct FLocalizedMessage, and it has a codec");
    BunchWriter w4;
    CHECK(write_struct("FLocalizedMessage", Value(econ), w4) && w4.bit_pos() == 336 && !write_struct("FLocalizedMessage", Value::u64(1), w4),
          "write_struct reaches it by name (and takes no other kind of value)");
    CHECK(zero_struct("FLocalizedMessage") && zero_struct("FLocalizedMessage")->get_if<LocalizedMessage>(), "its zero value is the empty message");
    // refusals
    Bits args = empty;
    args.set(64, 1, 32);
    CHECK(!read_msg(args), "a message with a format argument is refused (no capture shows an argument's layout)");
    Bits cut = Bits(w2).slice(0, 300);
    CHECK(!read_msg(cut), "a truncated message is refused");
    bool all_cuts = true;
    for (size_t n = 0; n < 336; ++n) all_cuts = all_cuts && !read_msg(Bits(w2).slice(0, n));
    CHECK(all_cuts, "every truncation of Economy / UnequipFailCorruption is refused");
    BunchWriter nul;
    CHECK(!write_localized_message(nul, LocalizedMessage{std::string("a\0b", 3), "k"}) && nul.bit_pos() == 0, "a string with a NUL is refused, nothing written");
    BunchWriter bad_utf8;
    CHECK(!write_localized_message(bad_utf8, LocalizedMessage{"ok", "\xff"}) && bad_utf8.bit_pos() == 0, "a string that is not UTF-8 is refused");
    BunchWriter wide;
    CHECK(write_localized_message(wide, LocalizedMessage{"Tabelle", "Gr\xc3\xbc\xc3\x9f" "e"}), "a string with non-ASCII text is written as UTF-16, as every FString");
    const auto wide_back = read_msg(Bits(wide));
    CHECK(wide_back && wide_back->key == "Gr\xc3\xbc\xc3\x9f" "e", "...and reads back");
    Bits huge = empty;
    huge.set(0, 0x7fffffffu, 32);
    CHECK(!read_msg(huge), "a hostile string length is refused");
}
// (CoA) (C.Y.B)

// ---- hostile input ---------------------------------------------------------------------------------------------------------

static void test_fuzz() {
    using namespace capture_mount;
    std::printf("hostile input\n");
    Rng rng(0x30A75E71ull);
    size_t tried = 0, read = 0, stable = 0;
    const schema::ClassSchema& pc = schema_named("AoCPlayerControllerBP_C");
    auto run = [&](const schema::ClassSchema& s, const Payload& p, bool rep) {
        for (int m = 0; m < 12; ++m) {
            Bits bits = bits_of_hex(p.hex, p.bits);
            if (bits.size() == 0) return;
            switch (m % 6) {
                case 0: { const size_t at = rng.below(bits.size()); bits.b[at] = !bits.b[at]; break; }
                case 1: bits.erase(bits.size() / 2, 1 + rng.below(bits.size() / 2)); break;
                case 2: bits.append(rng.next(), 61); break;
                case 3: bits.set(rng.below(bits.size() > 32 ? bits.size() - 32 : 1), 0xffffffffu, 32 < static_cast<int>(bits.size()) ? 32 : 1); break;
                case 4: bits.set(rng.below(bits.size() > 16 ? bits.size() - 16 : 1), 0, 16 < static_cast<int>(bits.size()) ? 16 : 1); break;
                default: bits = bits.slice(0, rng.below(bits.size() + 1)); break;
            }
            ++tried;
            Read r;
            const bool ok = with_exact(bits, [&](const uint8_t* d, size_t, size_t n) { return n > 0 && read_block(s, d, n, rep, r); });
            if (!ok) continue;
            ++read;
            const auto again = write_block(s, r, rep);
            if (!again) { ++stable; continue; }
            Read back;
            if (with_exact(*again, [&](const uint8_t* d, size_t, size_t n) { return read_block(s, d, n, rep, back); }) && back.state == r.state && back.arrays.size() == r.arrays.size()) ++stable;
        }
    };
    for (const Callout& c : kCallouts) run(pc, kPayloads[c.payload], c.rep);
    for (const Donor& d : kDonors) {
        for (uint32_t i = 0; i < d.blocks; ++i) {
            const Block& b = kBlocks[d.first_block + i];
            if (const schema::ClassSchema* s = gen::find_class_schema(b.cls)) run(*s, kPayloads[b.payload], b.rep);
        }
    }
    std::printf("    %zu mutated blocks, %zu read, %zu of them write back to bits that read equal\n", tried, read, stable);
    CHECK(tried > 500, "the fuzz ran on exact-size heap buffers without a crash");
    CHECK(stable == read, "whatever reads writes back to bits that read equal");
}

int main() {
    test_codec();
    test_donors();
    test_callouts();
    test_fuzz();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
