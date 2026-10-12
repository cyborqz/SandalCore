// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_fast_array.cpp
//
//  protocol/repl/fast_array.{h,cpp} against the fast arrays the original server
//  sent (capture_blocks.h, fixtures/replay_data.bin):
//    - every captured field loop reads to its last bit (read_fast_array_fields)
//      and the arrays it gives write back the exact bits (write_fast_array_fields
//      into fresh shadows): the pawn's StatsComponent at possession (fields 3, 5,
//      6 and 10, 335 + 96 + 11 + 1 items, four of the Owner items with Base and
//      EquipmentValue), two NPCs', UPlayerInfo HeldItems (pkt#107), and four of the
//      GameState open's fast maps (fields 25, 26, 28, 29); and, RepLayout block
//      then fast fields, the whole UPlayerInfo block of pkt#107;
//    - updates after an open, from the shadow: the pawn's StatRepInt32Owner and
//      CurrentEffects chain (pkt#8228, #8240, #8301, #8320: items changed, then
//      added and deleted) and an NPC's CurrentEffects delete + add, which the
//      server sent three times (pkt#556, #882, #1722) because its packets were
//      never acknowledged: a NAK's restored shadow writes the same bits again;
//    - the shadow on small arrays built from scratch: nothing changed writes
//      nothing, one changed item sends that item alone, a removal sends its ID as
//      deleted, an addition sends the new ID;
//    - refusals: values that do not fit a layout, too many changes, truncated or
//      malformed payloads, a field loop that is not all fast arrays, the
//      struct-delta array that is not implemented;
//    - the parent mask (per-connection conditions), the undo record of a write, a write
//      that changes nothing copying no shadow, the client's application of a deletion.
//
//  Also run once as a scratch sweep (not kept): all 847 supported fast-array
//  payloads of the whole capture (every StatsComponent array of the pawn and the
//  NPC channels, HeldItems, the GameState maps) read to their last bit and write
//  back the exact bits.
// ============================================================================
#include "capture_blocks.h"
#include "protocol/repl/fast_array.h"
#include "protocol/repl/rep_block.h"
#include "protocol/schema/generated/index.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::BunchWriter;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
using capture_blocks::Block;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static const schema::ClassSchema& schema_named(const char* name) {
    const schema::ClassSchema* s = gen::find_class_schema(name);
    if (!s) { std::printf("  [FAIL] no schema %s\n", name); std::exit(2); }
    return *s;
}

static const schema::ClassSchema& schema_of(const Block& b) { return schema_named(b.class_name); }

/// The client's stat table is not on the wire: FStatInt32InstRep carries Base and
/// EquipmentValue for the Records it marks.  The captured pawn's four such stats
/// (Owner items 387, 390, 668 and 669) have Records 0x5429e8c0xxxxxxxx and
/// 0x5429e8c4xxxxxxxx; the parse of all 371 + 5 + 1 captured Int32 arrays to
/// their last bit needs exactly these two prefixes.
static CodecOptions stat_options() {
    CodecOptions o;
    o.stat_int32_extended = [](int64_t record) {
        const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(record) >> 32);
        return hi == 0x5429e8c0u || hi == 0x5429e8c4u;
    };
    return o;
}

/// `n` bits of `bytes` from bit `start`, as a buffer starting at bit 0.
static std::vector<uint8_t> slice_bits(const std::vector<uint8_t>& bytes, size_t start, size_t n) {
    std::vector<uint8_t> out((n + 7) / 8, 0);
    for (size_t i = 0; i < n; ++i) out[i >> 3] |= ((bytes[(start + i) >> 3] >> ((start + i) & 7)) & 1) << (i & 7);
    return out;
}
// -- C O A x Cyb --

/// True when `w` holds exactly the `n` bits of `expect` from bit `from`.
static bool same_as(const BunchWriter& w, const std::vector<uint8_t>& expect, size_t from, size_t n) {
    if (w.bit_pos() != n) {
        std::printf("    wrote %zu bits, expected %zu\n", w.bit_pos(), n);
        return false;
    }
    const auto want = slice_bits(expect, from, n);
    size_t diff = 0;
    if (!capture_blocks::same_bit_range(w.data(), want.data(), n, &diff)) {
        std::printf("    first differing bit %zu\n", diff);
        return false;
    }
    return true;
}

/// The fields of a block's field loop from bit `from`, read and applied to fresh arrays.
struct Rebuilt {
    std::vector<uint8_t>         bytes;
    std::vector<FastArrayField>  fields;
    FastArrays                   arrays;
    bool                         ok = false;
    size_t                       end_pos = 0;
};

static Rebuilt rebuild(const Block& b, size_t from, const CodecOptions& opts) {
    Rebuilt r;
    r.bytes = capture_blocks::bytes_of(b.hex);
    const auto& s = schema_of(b);
    size_t pos = from;
    auto fields = read_fast_array_fields(s, r.bytes.data(), b.bits, pos, opts);
    r.end_pos = pos;
    if (!fields) return r;
    r.fields = std::move(*fields);
    for (const FastArrayField& f : r.fields) {
        FastArrayState* st = r.arrays.get(s, f.field);
        if (!st || !apply_fast_array_delta(*st, f.delta)) return r;
    }
    r.ok = true;
    return r;
}

static std::vector<uint32_t> field_numbers(const Rebuilt& r) {
    std::vector<uint32_t> v;
    for (const FastArrayField& f : r.fields) v.push_back(f.field);
    return v;
}

/// Read to the last bit, and the arrays it gives write back the exact bits.
static Rebuilt round_trip(const Block& b, size_t from, const std::vector<uint32_t>& fields, const CodecOptions& opts) {
    const std::string name = std::string(b.name) + " (" + b.class_name + ", " + std::to_string(b.bits - from) + " bits)";
    Rebuilt r = rebuild(b, from, opts);
    CHECK(r.ok && r.end_pos == b.bits, (name + ": the field loop reads to the last bit").c_str());
    CHECK(field_numbers(r) == fields, (name + ": the fields are the ones expected").c_str());
    if (!r.ok) return r;
    BunchWriter w;
    FastArrayShadows shadows;
    bool failed = false;
    const size_t n = write_fast_array_fields(schema_of(b), r.arrays, shadows, w, opts, &failed);
    CHECK(!failed && n == b.bits - from && same_as(w, r.bytes, from, n),
          (name + ": the rebuilt arrays write back the exact bits").c_str());
    const size_t before = w.bit_pos();
    CHECK(write_fast_array_fields(schema_of(b), r.arrays, shadows, w, opts) == 0 && w.bit_pos() == before,
          (name + ": a second write of the same arrays writes nothing").c_str());
    return r;
}

static const FastArrayField* field_of(const Rebuilt& r, uint32_t field) {
    for (const FastArrayField& f : r.fields) {
        if (f.field == field) return &f;
    }
    return nullptr;
}

static ElementState stat_item(int64_t record, uint32_t value) {
    ElementState es;
    StatInt32Inst t;
    t.record = record;
    t.value = value;
    es.values[1] = Value(t);
    return es;
}

static void test_layouts() {
    std::printf("layouts of the fast-array fields\n");
    const auto& stats = schema_named("UAoCStatsComponent");
    const auto owner = fast_array_layout(stats, 3);
    CHECK(owner && owner->field == 3 && owner->element.size() == 1 &&
              std::string(owner->element[0].struct_name) == "FStatInt32InstRep",
          "StatsComponent field 3 (StatRepInt32Owner): one item command, the NetSerialize FStatInt32InstRep");
    const auto by_name = fast_array_layout(stats, "StatRepFloatArrayEveryoneProxy");
    CHECK(by_name && by_name->field == 10 && by_name->element.size() == 2 && by_name->element[1].type == schema::PropType::DynamicArray,
          "field 10 by name (StatRepFloatArrayEveryoneProxy): Record and Values, a nested array");
    const auto effects = fast_array_layout(stats, 11);
    CHECK(effects && effects->element.size() == 16, "field 11 (CurrentEffects): 16 item commands");
    CHECK(!fast_array_layout(stats, 0) && !fast_array_layout(stats, 12), "bReplicates and the plain struct CrowdControlState are not fast arrays");
    CHECK(!fast_array_layout(stats, 13) && !fast_array_layout(stats, 22) && !fast_array_layout(stats, 99),
          "an RPC field and a field beyond the class are not fast arrays");
    CHECK(!fast_array_layout(stats, "NoSuchProperty"), "an unknown name is not one either");

    const auto& info = schema_named("UPlayerInfo");
    const auto held = fast_array_layout(info, 28);
    CHECK(held && held->element.size() == 3, "UPlayerInfo field 28 (HeldItems): Key, ItemGuid, EnchantmentLevel");
    const auto& gs = schema_named("AoCGameStateBP_C");
    const auto citizens = fast_array_layout(gs, "NodeNumberOfCitizensFastMap");
    CHECK(citizens && citizens->field == 29 && citizens->element.size() == 2, "GameState field 29 (NodeNumberOfCitizensFastMap): Value and Key");

    CHECK(gs.field("ReplicatedGlobalQuestCache") && !fast_array_layout(gs, "ReplicatedGlobalQuestCache"),
          "ReplicatedGlobalQuestCache (struct-delta items, not implemented) is refused, not written in the wrong form");
// C-O-A.Cyb

    FastArrays arrays;
    CHECK(arrays.get(stats, 3) != nullptr && arrays.get(stats, 3) == arrays.get(stats, "StatRepInt32Owner"),
          "FastArrays::get creates the array of a fast-array field once, by index or name");
    CHECK(arrays.get(stats, 0) == nullptr && arrays.get(stats, 13) == nullptr && arrays.all().size() == 1,
          "and none for a field that is not one");
}

static void test_stat_codec() {
    std::printf("FStatInt32InstRep (client exe 0x146164bf0)\n");
    const CodecOptions opts = stat_options();
    StatInt32Inst plain;
    plain.record = 0x63727e7756de;
    plain.value = 0x40200000;
    BunchWriter w;
    CHECK(write_struct("FStatInt32InstRep", Value(plain), w) && w.bit_pos() == 96, "a plain stat is Record + Value: 96 bits");
    wire::PacketReader r(w.data(), w.byte_size(), w.bit_pos());
    const auto back = read_struct("FStatInt32InstRep", r, opts);
    CHECK(back && *back == Value(plain) && r.pos() == 96, "and reads back");

    StatInt32Inst ext;
    ext.record = static_cast<int64_t>(0x5429e8c037770000ull);
    ext.value = 0x42dc0000;
    ext.extended = true;
    ext.base = 0x41200000;
    ext.equipment = 0x42c80000;
    BunchWriter we;
    CHECK(write_struct("FStatInt32InstRep", Value(ext), we) && we.bit_pos() == 160, "a stat the client's table marks adds Base and EquipmentValue: 160 bits");
    wire::PacketReader re(we.data(), we.byte_size(), we.bit_pos());
    const auto backe = read_struct("FStatInt32InstRep", re, opts);
    CHECK(backe && *backe == Value(ext) && re.pos() == 160, "and reads back with the table");
    wire::PacketReader rn(we.data(), we.byte_size(), we.bit_pos());
    const auto without = read_struct("FStatInt32InstRep", rn, CodecOptions{});
    CHECK(without && rn.pos() == 96 && !(*without == Value(ext)), "without the table the reader stops after Value: the wire does not say");

    StatInt32Inst inconsistent = plain;
    inconsistent.base = 1;   // Base without `extended`
    CHECK(is_known_struct("FStatInt32InstRep") && struct_accepts("FStatInt32InstRep", Value(ext)) &&
              !struct_accepts("FStatInt32InstRep", Value(inconsistent)) && !struct_accepts("FStatInt32InstRep", Value::u64(1)),
          "struct_accepts refuses base / equipment on a plain stat and other value kinds");
    BunchWriter wb;
    CHECK(!write_struct("FStatInt32InstRep", Value(inconsistent), wb) && wb.bit_pos() == 0, "and the writer refuses them");
}

static void test_captured_field_loops() {
    std::printf("captured field loops: read to the last bit, written back bit for bit\n");
    const CodecOptions opts = stat_options();

    // The pawn's StatsComponent at possession (ch 19 pkt#107).
    {
        const Rebuilt r = round_trip(capture_blocks::kStatsPawn107, 0, {3, 5, 6, 10}, opts);
        if (r.ok) {
            const FastArrayField* owner = field_of(r, 3);
            const FastArrayField* proxy = field_of(r, 5);
            const FastArrayField* everyone = field_of(r, 6);
            const FastArrayField* floats = field_of(r, 10);
            CHECK(owner && owner->delta.changed.size() == 335 && owner->delta.deleted.empty() &&
                      owner->delta.array_key == 417 && owner->delta.base_key == 0 && owner->delta.supports_delta,
                  "StatRepInt32Owner: 335 items, key 417 against base 0, the leading bit set");
            CHECK(proxy && proxy->delta.changed.size() == 96 && proxy->delta.array_key == 169 && proxy->delta.base_key == 0,
                  "StatRepInt32OwnerProxy: 96 items, key 169");
            CHECK(everyone && everyone->delta.changed.size() == 11 && everyone->delta.array_key == 21,
                  "StatRepInt32EveryoneProxy: 11 items, key 21");
            CHECK(floats && floats->delta.changed.size() == 1 && floats->delta.array_key == 2,
                  "StatRepFloatArrayEveryoneProxy: 1 item, key 2");
            if (owner) {
                const auto& c = owner->delta.changed;
                const auto* first = c.empty() ? nullptr : c[0].values.values.at(1).get_if<StatInt32Inst>();
                CHECK(!c.empty() && c[0].id == 2 && first && first->record == 0x634d499c567c && first->value == 0x41f00000 && !first->extended,
                      "the first Owner item: ID 2, Record 0x634d499c567c, Value 30.0f, no Base");
                size_t extended = 0;
                bool ids_ok = true;
                for (const auto& item : c) {
                    const auto* t = item.values.values.at(1).get_if<StatInt32Inst>();
                    if (t && t->extended) {
                        ++extended;
                        ids_ok = ids_ok && (item.id == 387 || item.id == 390 || item.id == 668 || item.id == 669);
                    }
                }
                CHECK(extended == 4 && ids_ok, "four Owner items (IDs 387, 390, 668, 669) carry Base and EquipmentValue");
                for (const auto& item : c) {
                    if (item.id != 387) continue;
                    const auto* t = item.values.values.at(1).get_if<StatInt32Inst>();
                    CHECK(t && t->extended && t->record == static_cast<int64_t>(0x5429e8c037770000ull) && t->value == 0x42dc0000 &&
                              t->base == 0x41200000 && t->equipment == 0x42c80000,
                          "item 387: Value 110.0f, Base 10.0f, EquipmentValue 100.0f");
                }
                bool array_order = c.size() > 12 && c[10].id == 398 && c[12].id == 24;
                CHECK(array_order, "the items come in the array's order (IDs 2, 4, 6 ... 20, 398, 399, 24, ...), not by ID");
            }
            // Without the stat table the Owner array does not parse.
            size_t pos = 0;
            CHECK(!read_fast_array_fields(schema_of(capture_blocks::kStatsPawn107), r.bytes.data(), capture_blocks::kStatsPawn107.bits, pos),
                  "without the stat table the pawn's Owner array does not read (four items are 64 bits longer)");
            CHECK(pos == 0, "and pos stays");
        }
    }

    // An NPC's StatsComponent open (ch 5 pkt#26): fields 6 and 10.
    round_trip(capture_blocks::kStatsNpc5, 0, {6, 10}, opts);
// CoA / C Y B

    // An NPC's StatsComponent open with CurrentEffects (ch 21 pkt#100).
    {
        const Rebuilt r = round_trip(capture_blocks::kStatsNpc21Open, 0, {6, 10, 11}, opts);
        const FastArrayField* fx = field_of(r, 11);
        CHECK(fx && fx->delta.array_key == 1927 && fx->delta.changed.size() == 2 && fx->delta.changed[0].id == 1 && fx->delta.changed[1].id == 643,
              "CurrentEffects: key 1927 against base 0, items 1 and 643");
    }

    // UPlayerInfo HeldItems, the field after the RepLayout block of the pawn's BaseCharacterInfo (ch 19 pkt#107).
    {
        const Block& b = capture_blocks::kPlayerInfoPossessed;
        const Rebuilt r = round_trip(b, b.rep_bits, {28}, opts);
        const FastArrayField* held = field_of(r, 28);
        CHECK(held && held->delta.changed.size() == 23 && held->delta.array_key == 46 && held->delta.changed[0].id == 1,
              "HeldItems: 23 items, key 46, IDs 1 to 23");
        if (held) {
            const auto& v = held->delta.changed[0].values.values;
            CHECK(v.at(1) == Value::u64(15) && v.at(2) == Value::i64(6064631131688402948ll) && v.at(3) == Value::u64(0),
                  "item 1: EEquipSlot 15, ItemGuid 6064631131688402948, EnchantmentLevel 0");
        }
    }

    // The GameState open's fast maps (ch 6 pkt#28): 244 + 46 + 26 + 14 items.
    {
        const Rebuilt r = round_trip(capture_blocks::kGameStateFastMaps28, 0, {25, 26, 28, 29}, opts);
        const FastArrayField* estates = field_of(r, 25);
        const FastArrayField* nodes = field_of(r, 26);
        const FastArrayField* percent = field_of(r, 28);
        const FastArrayField* citizens = field_of(r, 29);
        CHECK(estates && estates->delta.changed.size() == 244 && estates->delta.array_key == 10736,
              "EstateIDToOwnerGuid: 244 items (Key int64, Value a Guid of four int32), key 10736");
        CHECK(nodes && nodes->delta.changed.size() == 46 && nodes->delta.array_key == 230,
              "NodeIDToClientNodeData: 46 items (enums, a GuidNamePair with a string, a tag container ...), key 230");
        CHECK(percent && percent->delta.changed.size() == 26 && percent->delta.array_key == 50805 &&
                  percent->delta.changed[0].id == 1 && percent->delta.changed[0].values.values.at(1) == Value::u64(100) &&
                  percent->delta.changed[0].values.values.at(2) == Value::u64(8),
              "NodePercentLevelCompleteFastMap: 26 items of 48 bits (ID, Value byte, Key byte), item 1 is 100 for node 8");
        CHECK(citizens && citizens->delta.changed.size() == 14 && citizens->delta.array_key == 4461,
              "NodeNumberOfCitizensFastMap: 14 items of 72 bits (ID, Value byte, Key uint32)");
    }
}

/// The blocks of a chain: the arrays' state after the previous one, changed the way the game did.
static void expect_block(const char* what, const Block& b, const FastArrays& arrays, FastArrayShadows& shadows,
                         const CodecOptions& opts) {
    BunchWriter w;
    bool failed = false;
    const size_t n = write_fast_array_fields(schema_of(b), arrays, shadows, w, opts, &failed);
    const auto bytes = capture_blocks::bytes_of(b.hex);
    CHECK(!failed && n == b.bits && same_as(w, bytes, 0, n), what);
}

static void test_updates_from_the_shadow() {
    std::printf("captured updates after an open, written from the shadow\n");
    const CodecOptions opts = stat_options();
    const auto& stats = schema_named("UAoCStatsComponent");

    // ---- the pawn: StatRepInt32Owner (field 3) and CurrentEffects (field 11) -------------
    // The capture shows the Owner array at key 417 in pkt#107 and at base 690 in pkt#8228; what moved it
    // between is not in the capture (the server's states do not advance on a NAK, see below), so the
    // chain starts from the pkt#107 items with the key the first update names as its base.
    const Rebuilt open = rebuild(capture_blocks::kStatsPawn107, 0, opts);
    FastArrays arrays;
    FastArrayState* owner = arrays.get(stats, 3);
    FastArrayState* effects = arrays.get(stats, 11);
    CHECK(open.ok && owner && effects && apply_fast_array_delta(*owner, field_of(open, 3)->delta), "the pawn's Owner array, rebuilt from pkt#107");
    if (!open.ok || !owner || !effects) return;
    owner->set_array_key(690);

    FastArrayShadows shadows;
    {
        BunchWriter scratch;   // the send that put the connection at key 690: everything, against the default state
        const size_t n = write_fast_array_fields(stats, arrays, shadows, scratch, opts);
        CHECK(n > 0 && shadows.by_field.at(3).array_key == 690 && shadows.by_field.at(3).items.size() == 335 &&
                  shadows.by_field.count(11) == 0,
              "the connection's shadow is at key 690 with 335 items; the empty CurrentEffects (key 0) sent nothing");
    }

    const int64_t kRecord15 = 0x63727e7756de, kRecord19 = 0x63727e7756d4;
    const uint32_t kHigh = 0x40200000, kLow = 0x3f19999a;   // 2.5f and 0.6f: items 15 and 19 alternate between them

    // pkt#8228: both items change, and the first effect is added (ID 1, key 0 -> 1).
    {
        const Rebuilt upd = rebuild(capture_blocks::kStatsPawn8228, 0, opts);
        const FastArrayField* fx = upd.ok ? field_of(upd, 11) : nullptr;
        CHECK(fx && fx->delta.changed.size() == 1 && fx->delta.array_key == 1 && fx->delta.base_key == 0 && fx->delta.changed[0].id == 1,
              "pkt#8228 CurrentEffects: one item, ID 1, key 1 against base 0");
        CHECK(owner->change(15, stat_item(kRecord15, kHigh)) && owner->change(19, stat_item(kRecord19, kHigh)) && owner->array_key() == 692,
              "two MarkItemDirty calls move the Owner key 690 -> 692");
        CHECK(fx && effects->add(fx->delta.changed[0].values) == 1 && effects->array_key() == 1, "the effect gets ID 1, key 1");
        expect_block("pkt#8228: items 15 and 19 of Owner and the new effect, exactly as the original server sent", capture_blocks::kStatsPawn8228,
                     arrays, shadows, opts);
        BunchWriter w;
        CHECK(write_fast_array_fields(stats, arrays, shadows, w, opts) == 0 && w.bit_pos() == 0, "nothing more to send after it");
    }

    // pkt#8240: both items change back; the effect is removed (key 1 -> 3: two bumps).
    {
        CHECK(owner->change(15, stat_item(kRecord15, kLow)) && owner->change(19, stat_item(kRecord19, kLow)), "items 15 and 19 back to 0.6f");
        CHECK(effects->remove(1), "the effect goes");
        effects->mark_array_dirty();   // the original server's removals move the key by two (here 1 -> 3, then 4 -> 6)
        CHECK(effects->array_key() == 3, "CurrentEffects key 3");
        expect_block("pkt#8240: Owner 694 against 692 and the effect's ID 1 as deleted, exactly as sent", capture_blocks::kStatsPawn8240, arrays, shadows, opts);
    }
// -- C_Y_B . C.O.A --

    // pkt#8301: both change again; a new effect (ID 2 -- IDs are not reused -- key 3 -> 4).
    {
        const Rebuilt upd = rebuild(capture_blocks::kStatsPawn8301, 0, opts);
        const FastArrayField* fx = upd.ok ? field_of(upd, 11) : nullptr;
        CHECK(owner->change(15, stat_item(kRecord15, kHigh)) && owner->change(19, stat_item(kRecord19, kHigh)), "items 15 and 19 to 2.5f");
        CHECK(fx && effects->add(fx->delta.changed[0].values) == 2 && effects->array_key() == 4, "the second effect gets ID 2");
        expect_block("pkt#8301: Owner 696 against 694 and the effect with ID 2, exactly as sent", capture_blocks::kStatsPawn8301, arrays, shadows, opts);
    }

    // pkt#8320: back to 0.6f; the second effect is removed (key 4 -> 6).
    {
        CHECK(owner->change(15, stat_item(kRecord15, kLow)) && owner->change(19, stat_item(kRecord19, kLow)), "items 15 and 19 to 0.6f");
        CHECK(effects->remove(2), "the second effect goes");
        effects->mark_array_dirty();
        expect_block("pkt#8320: Owner 698 against 696 and the effect's ID 2 as deleted, exactly as sent", capture_blocks::kStatsPawn8320, arrays, shadows, opts);
        BunchWriter w;
        CHECK(write_fast_array_fields(stats, arrays, shadows, w, opts) == 0, "and the chain ends with nothing to send");
    }

    // ---- an NPC: CurrentEffects with a delete and an add in one payload, sent three times ----
    {
        const Rebuilt npc = rebuild(capture_blocks::kStatsNpc21Open, 0, opts);
        const Rebuilt upd = rebuild(capture_blocks::kEffectsNpc21_556, 0, opts);
        const FastArrayField* fx = upd.ok ? field_of(upd, 11) : nullptr;
        CHECK(npc.ok && fx && fx->delta.array_key == 1930 && fx->delta.base_key == 1927 && fx->delta.deleted == std::vector<int32_t>{643} &&
                  fx->delta.changed.size() == 1 && fx->delta.changed[0].id == 644,
              "the NPC's update: key 1930 against base 1927, ID 643 deleted, ID 644 changed");
        if (!npc.ok || !fx) return;
        FastArrays narrays;
        for (const FastArrayField& f : npc.fields) {
            CHECK(apply_fast_array_delta(*narrays.get(schema_of(capture_blocks::kStatsNpc21Open), f.field), f.delta), "the NPC's open applied");
        }
        FastArrayShadows nshadows;
        BunchWriter open_out;
        write_fast_array_fields(stats, narrays, nshadows, open_out, opts);   // the open: the connection now has key 1927
        FastArrayState* nfx = narrays.get(stats, 11);
        const FastArrayShadow at_open = nshadows.by_field.at(11);
        CHECK(at_open.array_key == 1927 && at_open.items.size() == 2, "the connection's CurrentEffects shadow: key 1927, two items");
        CHECK(nfx->remove(643), "the effect 643 is removed");
        nfx->mark_array_dirty();
        CHECK(nfx->add(fx->delta.changed[0].values) == 644 && nfx->array_key() == 1930, "the new effect gets ID 644 (the counter followed the loaded IDs), key 1930");
        const FastArrayShadows before_send = nshadows;
        expect_block("pkt#556: the delete of 643 and the add of 644, exactly as sent", capture_blocks::kEffectsNpc21_556, narrays, nshadows, opts);
        // The original server sent this block again in pkt#882 and pkt#1722, byte for byte: its packets were never
        // acknowledged, and a NAK puts the base state back to what it was before the send (FObjectReplicator::
        // ReceivedNak restores RecentCustomDeltaState from the retirement record).  A saved shadow does the same.
        nshadows = before_send;
        expect_block("after a NAK (the shadow put back): the identical block again, as pkt#882 and #1722 were", capture_blocks::kEffectsNpc21_556, narrays,
                     nshadows, opts);
    }
}

// ---- small arrays built from scratch --------------------------------------------------------

/// A NodeNumberOfCitizensFastMap item: Value (uint8), Key (uint32).
static ElementState citizens_item(uint32_t value, uint32_t key) {
    ElementState es;
    es.values[1] = Value::u64(value);
    es.values[2] = Value::u64(key);
    return es;
}

static std::vector<FastArrayField> read_written(const schema::ClassSchema& s, const BunchWriter& w) {
    size_t pos = 0;
    auto fields = read_fast_array_fields(s, w.data(), w.bit_pos(), pos);
    return fields ? *fields : std::vector<FastArrayField>{};
}

static void test_shadow() {
    std::printf("the shadow: what a write sends after a change\n");
    const auto& gs = schema_named("AoCGameStateBP_C");
    FastArrays arrays;
    FastArrayState* st = arrays.get(gs, "NodeNumberOfCitizensFastMap");
    if (!st) { CHECK(false, "the GameState fast map"); return; }

    CHECK(st->array_key() == 0 && st->items().empty(), "a new array: key 0, no items");
    {
        BunchWriter w;
        FastArrayShadows sh;
        CHECK(write_fast_array_fields(gs, arrays, sh, w) == 0 && w.bit_pos() == 0 && sh.by_field.empty(),
              "an array nobody touched (key 0, the default state's key) sends nothing");
    }
    const auto a = st->add(citizens_item(10, 1));
    const auto b = st->add(citizens_item(20, 2));
    const auto c = st->add(citizens_item(30, 3));
    CHECK(a == 1 && b == 2 && c == 3 && st->array_key() == 3, "three adds: IDs 1, 2, 3 (++IDCounter), the array key 3");
    CHECK(st->find(2) && st->find(2)->key == 0 && !st->find(9), "an added item's ReplicationKey is 0 (INDEX_NONE + 1)");

    FastArrayShadows conn;   // one connection
    BunchWriter w1;
    bool failed = false;
    const size_t n1 = write_fast_array_fields(gs, arrays, conn, w1, {}, &failed);
    {
        const auto f = read_written(gs, w1);
        CHECK(!failed && f.size() == 1 && f[0].field == 29 && f[0].delta.array_key == 3 && f[0].delta.base_key == 0 &&
                  f[0].delta.deleted.empty() && f[0].delta.changed.size() == 3 && f[0].delta.changed[0].id == 1 && f[0].delta.changed[2].id == 3,
              "the first send: key 3 against base 0, all three items in array order");
        CHECK(n1 == w1.bit_pos() && n1 > 129 + 3 * 72, "its payload is 129 + 3 x (32 + 8 + 32) bits, after the field header");
    }
    {
        BunchWriter w;
        CHECK(write_fast_array_fields(gs, arrays, conn, w) == 0 && w.bit_pos() == 0, "nothing changed: nothing written");
    }

    // One item changes: only it goes out.
    CHECK(st->change(2, citizens_item(21, 2)) && st->array_key() == 4 && st->find(2)->key == 1, "MarkItemDirty on item 2: its key 1, the array's 4");
    BunchWriter w2;
    write_fast_array_fields(gs, arrays, conn, w2);
    {
        const auto f = read_written(gs, w2);
        CHECK(f.size() == 1 && f[0].delta.array_key == 4 && f[0].delta.base_key == 3 && f[0].delta.deleted.empty() &&
                  f[0].delta.changed.size() == 1 && f[0].delta.changed[0].id == 2 && f[0].delta.changed[0].values.values.at(1) == Value::u64(21),
              "one changed item: key 4 against base 3, item 2 alone with its new value");
        BunchWriter payload;
        FastArrayShadow scratch;
        scratch.array_key = 3;
        scratch.items = {{1, 0}, {2, 0}, {3, 0}};
        CHECK(write_fast_array_delta(*st, scratch, payload) == 129 + 72, "and its payload is 129 + 72 bits");
    }
// (C | Y | B | CoA)

    // An item is removed: its ID goes out as deleted, nothing else.
    CHECK(st->remove(1) && st->array_key() == 5 && st->items().size() == 2 && st->items()[0].id == 2, "removing item 1 keeps the others in order, the key 5");
    BunchWriter w3;
    write_fast_array_fields(gs, arrays, conn, w3);
    {
        const auto f = read_written(gs, w3);
        CHECK(f.size() == 1 && f[0].delta.array_key == 5 && f[0].delta.base_key == 4 && f[0].delta.deleted == std::vector<int32_t>{1} &&
                  f[0].delta.changed.empty(),
              "a removal: ID 1 in the deleted list, no changed item");
    }

    // An item is added: a new ID, never a reused one.
    const auto d = st->add(citizens_item(40, 4));
    CHECK(d == 4 && st->array_key() == 6, "the next add gets ID 4, not the freed 1");
    BunchWriter w4;
    write_fast_array_fields(gs, arrays, conn, w4);
    {
        const auto f = read_written(gs, w4);
        CHECK(f.size() == 1 && f[0].delta.array_key == 6 && f[0].delta.base_key == 5 && f[0].delta.deleted.empty() &&
                  f[0].delta.changed.size() == 1 && f[0].delta.changed[0].id == 4 && f[0].delta.changed[0].values.values.at(2) == Value::u64(4),
              "an addition: the new item alone, ID 4");
    }

    // A removal and an addition in one interval: deleted first, then the new item.
    CHECK(st->remove(3) && st->add(citizens_item(50, 5)) == 5, "item 3 removed, item 5 added");
    BunchWriter w5;
    write_fast_array_fields(gs, arrays, conn, w5);
    {
        const auto f = read_written(gs, w5);
        CHECK(f.size() == 1 && f[0].delta.deleted == std::vector<int32_t>{3} && f[0].delta.changed.size() == 1 && f[0].delta.changed[0].id == 5 &&
                  f[0].delta.array_key == 8 && f[0].delta.base_key == 6,
              "both in one payload: ID 3 deleted, item 5 changed, key 8 against 6");
    }

    // An item added and removed between two sends is never seen.
    const auto tmp = st->add(citizens_item(60, 6));
    CHECK(tmp == 6 && st->remove(6), "item 6 added and removed before a send");
    BunchWriter w6;
    write_fast_array_fields(gs, arrays, conn, w6);
    {
        const auto f = read_written(gs, w6);
        CHECK(f.size() == 1 && f[0].delta.deleted.empty() && f[0].delta.changed.empty() && f[0].delta.array_key == 10 && f[0].delta.base_key == 8 &&
                  w6.bit_pos() > 129,
              "the keys moved, so a header goes out (UE sends it so the client can look for implicit deletes): no item in it");
    }

    // A second connection has nothing yet: it gets the whole array.
    {
        FastArrayShadows other;
        BunchWriter w;
        write_fast_array_fields(gs, arrays, other, w);
        const auto f = read_written(gs, w);
        CHECK(f.size() == 1 && f[0].delta.base_key == 0 && f[0].delta.array_key == 10 && f[0].delta.deleted.empty() &&
                  f[0].delta.changed.size() == st->items().size() && f[0].delta.changed[0].id == 2 && f[0].delta.changed[1].id == 4 &&
                  f[0].delta.changed[2].id == 5,
              "a new connection's first send: every current item (2, 4, 5), nothing deleted");
    }

    // A NAK restores the shadow: the same bytes again.
    {
        st->change(4, citizens_item(41, 4));
        const FastArrayShadows saved = conn;
        BunchWriter first, again;
        write_fast_array_fields(gs, arrays, conn, first);
        conn = saved;
        write_fast_array_fields(gs, arrays, conn, again);
        CHECK(first.bit_pos() > 0 && first.bit_pos() == again.bit_pos() && capture_blocks::same_bit_range(first.data(), again.data(), first.bit_pos(), nullptr),
              "a NAK puts the saved shadow back and the write repeats exactly");
    }

    // Deleted IDs come in the order of the previous send.
    {
        FastArrays arr2;
        FastArrayState* s2 = arr2.get(gs, 29);
        for (uint32_t i = 0; i < 5; ++i) s2->add(citizens_item(i, i));
        FastArrayShadows sh2;
        BunchWriter tmp1;
        write_fast_array_fields(gs, arr2, sh2, tmp1);
        s2->remove(4);
        s2->remove(2);
        BunchWriter w;
        write_fast_array_fields(gs, arr2, sh2, w);
        const auto f = read_written(gs, w);
        CHECK(f.size() == 1 && f[0].delta.deleted == (std::vector<int32_t>{2, 4}), "two removals: deleted in the order the shadow has them (2, 4), not the order they were removed");
    }

    // Key arithmetic like UE's.
    {
        FastArrays arr3;
        FastArrayState* s3 = arr3.get(gs, 29);
        s3->set_array_key(-2);
        s3->mark_array_dirty();
        CHECK(s3->array_key() == 0, "IncrementArrayReplicationKey skips INDEX_NONE: -2 -> 0");
        s3->set_array_key(INT32_MAX);
        s3->mark_array_dirty();
        CHECK(s3->array_key() == INT32_MIN, "and wraps like an int32");
    }
}

static void test_refusals() {
    std::printf("refusals\n");
    const auto& gs = schema_named("AoCGameStateBP_C");
    const auto& stats = schema_named("UAoCStatsComponent");
    FastArrays arrays;
    FastArrayState* st = arrays.get(gs, 29);
// CoA / C-Y-B

    // values that do not fit the layout
    ElementState too_few;
    too_few.values[1] = Value::u64(1);
    ElementState wrong_kind = citizens_item(1, 1);
    wrong_kind.values[1] = Value::str("x");
    ElementState too_wide = citizens_item(256, 1);   // Value is a byte
    ElementState extra = citizens_item(1, 1);
    extra.values[3] = Value::u64(0);
    CHECK(!st->add(too_few) && !st->add(wrong_kind) && !st->add(too_wide) && !st->add(extra) && st->items().empty() && st->array_key() == 0,
          "add refuses missing, mistyped, too wide and surplus values, leaving the state alone");
    CHECK(st->add(citizens_item(1, 1)) == 1 && !st->change(1, too_wide) && !st->change(7, citizens_item(1, 1)) && !st->remove(7) &&
              !st->insert(1, citizens_item(2, 2)) && !st->insert(kIndexNone, citizens_item(2, 2)),
          "change refuses a bad value or an unknown ID, remove an unknown ID, insert a taken or INDEX_NONE ID");
    CHECK(st->insert(40, citizens_item(4, 4)) && st->add(citizens_item(5, 5)) == 41, "insert takes a given ID and the counter follows it");

    // too many changed items: write refuses, changes nothing
    {
        FastArrays big;
        FastArrayState* bs = big.get(gs, 29);
        for (int i = 0; i < kMaxFastArrayChanged + 1; ++i) bs->add(citizens_item(1, static_cast<uint32_t>(i)));
        FastArrayShadows sh;
        BunchWriter w;
        bool failed = false;
        CHECK(write_fast_array_fields(gs, big, sh, w, {}, &failed) == 0 && failed && w.bit_pos() == 0 && sh.by_field.empty(),
              "more than 2048 changed items: refused, nothing written, no shadow made");
    }
    // all or nothing across arrays
    {
        FastArrays mix;
        FastArrayState* good = mix.get(gs, 28);
        ElementState pct;
        pct.values[1] = Value::u64(50);
        pct.values[2] = Value::u64(3);
        good->add(pct);
        FastArrayState* bad = mix.get(gs, 29);
        for (int i = 0; i < kMaxFastArrayChanged + 1; ++i) bad->add(citizens_item(1, static_cast<uint32_t>(i)));
        FastArrayShadows sh;
        BunchWriter w;
        w.write_bits(5, 3);
        bool failed = false;
        CHECK(write_fast_array_fields(gs, mix, sh, w, {}, &failed) == 0 && failed && w.bit_pos() == 3 && sh.by_field.empty(),
              "one array too big: the good array's field is not written either, `out` and the shadows are as they were");
    }
    // too many deletions
    {
        FastArrays arr;
        FastArrayState* s = arr.get(gs, 29);
        FastArrayShadow sh;
        for (int i = 1; i <= kMaxFastArrayDeleted + 1; ++i) sh.items.emplace_back(i, 0);
        sh.array_key = 5;
        s->set_array_key(6);
        BunchWriter w;
        bool failed = false;
        CHECK(write_fast_array_delta(*s, sh, w, {}, &failed) == 0 && failed && sh.array_key == 5 && sh.items.size() == 2049,
              "more than 2048 deleted IDs: refused, the shadow untouched");
    }
// ~ Cyb & C | O | A ~

    // reader refusals on a valid payload (the NPC's StatRepFloatArrayEveryoneProxy, pkt#26)
    const Block& b = capture_blocks::kStatsNpc5;
    const auto bytes = capture_blocks::bytes_of(b.hex);
    size_t pos = 0;
    auto good = read_fast_array_fields(stats, bytes.data(), b.bits, pos);
    CHECK(good && pos == b.bits, "control: the NPC's block reads");
    pos = 0;
    CHECK(!read_fast_array_fields(stats, bytes.data(), b.bits - 1, pos) && pos == 0, "a block cut by one bit does not read");
    pos = 0;
    // A payload read with another array's layout: field 6's payload (96-bit stat items) under field 10's name (Record + a nested array).
    {
        size_t p = 0;
        const auto h = read_field_header(stats, bytes.data(), b.bits, p);
        BunchWriter w;
        write_field_header(stats, 10, h->payload_bits, w);
        w.write_bit_range(bytes.data(), p, h->payload_bits);
        size_t q = 0;
        CHECK(!read_fast_array_fields(stats, w.data(), w.bit_pos(), q) && q == 0, "a payload under the wrong field (another layout) does not read");
    }
    // an RPC field in the loop
    {
        BunchWriter w;
        BunchWriter payload;
        payload.write_bits(0x2b, 6);
        write_field(stats, 13, payload, w);   // ApplyEffect
        size_t q = 0;
        CHECK(!read_fast_array_fields(stats, w.data(), w.bit_pos(), q) && q == 0, "an RPC field in the loop: not a fast-array field loop");
    }
    // a non-fast property field
    {
        BunchWriter w;
        BunchWriter payload;
        payload.write_bit(1);
        write_field(stats, 12, payload, w);   // CrowdControlState
        size_t q = 0;
        CHECK(!read_fast_array_fields(stats, w.data(), w.bit_pos(), q), "a plain struct property's field: refused too");
    }
    // NumChanged beyond the limit
    {
        const auto layout = fast_array_layout(gs, 29);
        BunchWriter w;
        w.write_bit(1);
        w.write_int32(1);
        w.write_int32(0);
        w.write_int32(0);
        w.write_int32(kMaxFastArrayChanged + 1);
        for (int i = 0; i < 4000; ++i) w.write_int32(0);
        size_t q = 0;
        CHECK(!read_fast_array_delta(*layout, w.data(), w.bit_pos(), q) && q == 0, "NumChanged 2049 is refused (net.MaxNumberOfAllowedTArrayChangesPerUpdate)");
        BunchWriter neg;
        neg.write_bit(1);
        neg.write_int32(1);
        neg.write_int32(0);
        neg.write_int32(-1);
        neg.write_int32(0);
        q = 0;
        CHECK(!read_fast_array_delta(*layout, neg.data(), neg.bit_pos(), q), "a negative NumDeletes is refused");
        BunchWriter lie;
        lie.write_bit(1);
        lie.write_int32(1);
        lie.write_int32(0);
        lie.write_int32(0);
        lie.write_int32(5);   // five items announced, none there
        q = 0;
        CHECK(!read_fast_array_delta(*layout, lie.data(), lie.bit_pos(), q), "a count larger than the payload is refused");
    }
    // apply: an unknown deleted ID is skipped, a bad item refuses the whole delta
    {
        FastArrays arr;
        FastArrayState* s = arr.get(gs, 29);
        s->add(citizens_item(1, 1));
        FastArrayDelta d;
        d.array_key = 9;
        d.deleted = {77};
        CHECK(apply_fast_array_delta(*s, d) && s->items().size() == 1 && s->array_key() == 9, "a delta deleting an unknown ID is applied (skipped), as the client does");
        d.changed.push_back({5, citizens_item(300, 1)});
        CHECK(!apply_fast_array_delta(*s, d) && s->array_key() == 9 && s->items().size() == 1, "a delta with an item that does not fit is refused whole");
    }
}

/// A content block as an actor open assembles it: the RepLayout block (rep_block.h), then the fast-array
/// fields (UPlayerInfo at possession, ch 19 pkt#107: 165 bits of RepLayout, then HeldItems).
static void test_block_assembly() {
    std::printf("a content block: RepLayout block, then the fast-array fields\n");
    const Block& b = capture_blocks::kPlayerInfoPossessed;
    const auto& s = schema_of(b);
    const auto bytes = capture_blocks::bytes_of(b.hex);
    size_t pos = 0;
    const auto state = read_rep_block(s, bytes.data(), b.bits, pos);
    CHECK(state && pos == b.rep_bits, "the RepLayout block reads (165 bits) and leaves pos at the field loop");
    const auto fields = state ? read_fast_array_fields(s, bytes.data(), b.bits, pos) : std::nullopt;
    CHECK(fields && fields->size() == 1 && pos == b.bits, "the field loop reads to the end of the block");
    if (!state || !fields) return;
    FastArrays arrays;
    for (const FastArrayField& f : *fields) apply_fast_array_delta(*arrays.get(s, f.field), f.delta);
    BunchWriter w;
    RepShadow rep_shadow;
    FastArrayShadows fast_shadows;
    const size_t rep_bits = write_rep_block(s, *state, rep_shadow, w);
    const size_t fast_bits = write_fast_array_fields(s, arrays, fast_shadows, w);
    CHECK(rep_bits == b.rep_bits && fast_bits == b.bits - b.rep_bits && same_as(w, bytes, 0, b.bits),
          "write_rep_block then write_fast_array_fields give the block's 2868 bits exactly");
}

// ---- T6: apply_fast_array_delta removes what the delta deletes ----------------------------------------------------------------
static void t6_apply_deletions() {
    std::printf("apply_fast_array_delta and deletions\n");
    const auto& gs = schema_named("AoCGameStateBP_C");
    FastArrays arrays;
    FastArrayState* st = arrays.get(gs, "NodeNumberOfCitizensFastMap");
    ElementState a, b;
    a.values[1] = Value::u64(1); a.values[2] = Value::u64(1);
    b.values[1] = Value::u64(2); b.values[2] = Value::u64(2);
    const bool added = st && st->add(a) && st->add(b);
    FastArrayDelta d;
    d.array_key = 9;
    d.deleted = {1};
    CHECK(added && apply_fast_array_delta(*st, d) && st->items().size() == 1 && !st->find(1) && st->find(2),
          "a delta deleting ID 1 leaves only ID 2");
}

// ---- allocation counting: a write that changes nothing does not copy the shadow ---------------------------------------------------
static size_t g_allocated = 0;   // bytes the allocator was asked for since it was last reset

void* operator new(size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    g_allocated += n;
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

// ---- per-connection conditions and the undo record ----------------------------------------------------------------------------------
static void test_active_parents() {
    std::printf("fast arrays: the parent mask, the undo record, no copy when nothing changed\n");
    const auto& gs = schema_named("AoCGameStateBP_C");
    FastArrays arrays;
    FastArrayState* st = arrays.get(gs, "NodeNumberOfCitizensFastMap");
    if (!st) { CHECK(false, "the GameState fast map"); return; }
    st->add(citizens_item(10, 1));
    st->add(citizens_item(20, 2));
    st->add(citizens_item(30, 3));

    ParentMask mask;
    CHECK(mask.set_active(gs, "NodeNumberOfCitizensFastMap.Items", false), "the fast array's property is masked by the path of its Items command");
    FastArrayShadows conn;
    BunchWriter w;
    bool failed = true;
    CHECK(write_fast_array_fields(gs, arrays, conn, w, {}, &failed, &mask) == 0 && !failed && w.bit_pos() == 0 && conn.by_field.empty(),
          "an inactive parent's array is not written, and its shadow is untouched");

    mask.reset();
    FastArrayShadows plain;
    BunchWriter whole;
    write_fast_array_fields(gs, arrays, plain, whole);
    BunchWriter sent;
    FastArrayShadowsUndo undo;
    const size_t n = write_fast_array_fields(gs, arrays, conn, sent, {}, &failed, &mask, &undo);
    CHECK(n > 0 && !failed && sent.bit_pos() == whole.bit_pos() && capture_blocks::same_bit_range(sent.data(), whole.data(), whole.bit_pos(), nullptr),
          "once active it is sent in full, exactly as without a mask");
    CHECK(undo.entries.size() == 1 && undo.entries[0].first == 29 && !undo.entries[0].second, "the undo record has the field, which the connection did not have");
    undo.apply(conn);
    CHECK(conn.by_field.empty(), "applying it forgets the send");

    write_fast_array_fields(gs, arrays, conn, sent);   // the connection has it now
    mask.set_active(gs, "NodeNumberOfCitizensFastMap.Items", false);
    st->change(2, citizens_item(21, 2));
    BunchWriter quiet;
    CHECK(write_fast_array_fields(gs, arrays, conn, quiet, {}, &failed, &mask) == 0 && conn.by_field.at(29).array_key == 3,
          "a change while inactive sends nothing and leaves the shadow at key 3");
    mask.set_active(gs, "NodeNumberOfCitizensFastMap.Items", true);
    BunchWriter delta;
    FastArrayShadowsUndo undo2;
    write_fast_array_fields(gs, arrays, conn, delta, {}, &failed, &mask, &undo2);
    const auto f = read_written(gs, delta);
    CHECK(f.size() == 1 && f[0].delta.array_key == 4 && f[0].delta.base_key == 3 && f[0].delta.changed.size() == 1 && f[0].delta.changed[0].id == 2,
          "reactivated: the delta against the shadow, item 2 alone");
    undo2.apply(conn);
    CHECK(conn.by_field.at(29).array_key == 3 && conn.by_field.at(29).items.size() == 3, "and its undo puts the old shadow back");
// C-O-A and C_Y_B

    // A big shadow is not copied to find out that nothing changed.
    for (int i = 4; i <= 1500; ++i) st->add(citizens_item(i & 0x7F, i));
    FastArrayShadows big;
    BunchWriter first;
    write_fast_array_fields(gs, arrays, big, first);
    g_allocated = 0;
    {
        const FastArrayShadows copy = big;
        (void)copy;
    }
    const size_t copy_cost = g_allocated;
    g_allocated = 0;
    BunchWriter nothing;
    const size_t bits = write_fast_array_fields(gs, arrays, big, nothing);
    const size_t noop_cost = g_allocated;
    std::printf("    copying the shadow of 1500 items allocates %zu bytes, a write that changes nothing %zu\n", copy_cost, noop_cost);
    CHECK(bits == 0 && copy_cost > 8000 && noop_cost * 8 < copy_cost, "a write that changes nothing allocates a fraction of what copying the shadow would");
}

int main() {
    test_layouts();
    test_stat_codec();
    test_captured_field_loops();
    test_updates_from_the_shadow();
    test_block_assembly();
    test_shadow();
    test_refusals();
    t6_apply_deletions();
    test_active_parents();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
