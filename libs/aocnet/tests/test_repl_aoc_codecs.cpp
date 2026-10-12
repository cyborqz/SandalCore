// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_aoc_codecs.cpp
//
//  The NetSerialize codecs for AoC's own structs (protocol/repl/codecs.h) bit
//  for bit against what the original server sent (capture_aoc_codecs.h,
//  fixtures/replay_data.bin), each at the client exe's function:
//    - FQuat (0x141438440): the identity of a captured TargetTransforms element,
//      quat_to_wire / quat_from_wire (UE's save and load, checked by value),
//      and two whole ReplicatedActiveAbilities blocks: an open (RepLayout block,
//      then the field) through read_rep_block / read_fast_array_fields and back
//      through write_rep_block / write_fast_array_fields, and an update through
//      read_fast_array_fields and write_fast_array_delta;
//    - FGuidToBoolWrapper (0x145a7d730): three captured values (1, 2 and 20
//      GUIDs) and a whole InteractVisibleStateMap payload through
//      read_fast_array_delta / write_fast_array_delta with the item layout built
//      by hand (UInteractableComponent has no generated schema);
//    - FAlignmentContainer (0x145e77630): a captured value, and the whole
//      UAlignmentComponent blocks of an NPC and the pawn through read_rep_block /
//      write_rep_block; the InstanceTimers map, which no captured container
//      fills, by round trip;
//    - FAppearanceId (0x145919170): a captured value and the pawn's whole
//      AppearanceIDs block;
//    - FCharacterCustomizationSaveData (0x1463d6290): the pawn's whole 26,950-bit
//      Character Appearance block (2 decals, 2 blend groups, SectionsValues,
//      FaceMorphWeightMaps) and two NPC values, then every field kind by round
//      trip, the preset-known form (no Race / Gender bytes) and the refusals.
//  Every captured value is read to its last bit and written back to the same
//  bits.  The refusals: a TMap / TSet with a repeated key, a negative or
//  oversized count, a bool that is not 0 or 1, a delta wider than its field.
//
//  Also run once as a scratch sweep (not kept): every struct of the capture's
//  first 30,000 packets -- 6,547 FQuat, 373 FAlignmentContainer, 536
//  FAppearanceId, 84 FCharacterCustomizationSaveData, 38 FGuidToBoolWrapper --
//  reads to its last bit and writes back the exact bits; so do the whole blocks
//  of all 373 UAlignmentComponent, 6,547 UAoCAbilityComponent and 2
//  UCharacterAppearanceComponent (the RepLayout block through read_rep_block /
//  write_rep_block, the fast-array payloads through read_fast_array_delta and,
//  for the 3,281 without deletions, write_fast_array_delta against a shadow at
//  the payload's base key).  Every captured FQuat is the identity (X = Y = Z =
//  0): other values are by the exe's code and UE's source, not by the capture.
// ============================================================================
#include "capture_aoc_codecs.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/fast_array.h"
#include "protocol/repl/rep_block.h"
#include "protocol/schema/generated/index.h"
// [C O A - Cyb]

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::BunchWriter;
using wire::PacketReader;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
using capture_blocks::Block;
using capture_blocks::Slice;

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

// ---- helpers -----------------------------------------------------------------

/// Reads `s` as `name`; true when it consumed exactly s.bits and writing the value back gives the same
/// bits.  *out gets the value.
static bool slice_round_trip(const Slice& s, const char* name, Value* out, const CodecOptions& opts = {}) {
    const auto bytes = capture_blocks::bytes_of(s.hex);
    PacketReader r(bytes.data(), bytes.size(), s.bits);
    const auto v = read_struct(name, r, opts);
    if (!v || r.pos() != s.bits) return false;
    *out = *v;
    BunchWriter w;
    if (!write_struct(name, *v, w, opts) || w.bit_pos() != s.bits) return false;
    size_t diff = 0;
    if (!capture_blocks::same_bit_range(w.data(), bytes.data(), s.bits, &diff)) {
        std::printf("    first differing bit %zu\n", diff);
        return false;
    }
    return true;
}

/// Writes `v` as `name` and reads it back: the value and the bits it took (0 when either fails).
struct Trip {
    std::optional<Value> value;
    size_t               bits = 0;
    bool                 whole = false;   // the read consumed every bit written
};
static Trip trip(const char* name, const Value& v, const CodecOptions& opts = {}) {
    Trip t;
    BunchWriter w;
    if (!write_struct(name, v, w, opts)) return t;
    t.bits = w.bit_pos();
    PacketReader r(w.data(), w.byte_size(), w.bit_pos());
    t.value = read_struct(name, r, opts);
    t.whole = t.value && r.pos() == w.bit_pos();
    return t;
}

/// The value read from raw bits (nullopt for a refusal).
static std::optional<Value> read_bits_as(const char* name, const BunchWriter& w, const CodecOptions& opts = {}) {
    PacketReader r(w.data(), w.byte_size(), w.bit_pos());
    return read_struct(name, r, opts);
}

static GuidValue guid(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return GuidValue{a, b, c, d}; }

/// `n` bits of `bytes` from bit `start`, as a buffer starting at bit 0.
static std::vector<uint8_t> slice_bits(const std::vector<uint8_t>& bytes, size_t start, size_t n) {
    std::vector<uint8_t> out((n + 7) / 8, 0);
    for (size_t i = 0; i < n; ++i) out[i >> 3] |= ((bytes[(start + i) >> 3] >> ((start + i) & 7)) & 1) << (i & 7);
    return out;
}

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

/// A whole RepLayout-only block: read to its last bit, written back into an empty shadow to the same bits.
static std::optional<RepState> rep_block_round_trip(const Block& b, const CodecOptions& opts = {}) {
    const std::string name = std::string(b.name) + " (" + b.class_name + ", " + std::to_string(b.bits) + " bits)";
    const auto& s = schema_named(b.class_name);
    const auto bytes = capture_blocks::bytes_of(b.hex);
    size_t pos = 0;
    auto state = read_rep_block(s, bytes.data(), b.bits, pos, opts);
    CHECK(state && pos == b.bits, (name + ": reads to the last bit").c_str());
    if (!state) return std::nullopt;
    BunchWriter w;
    RepShadow shadow;
    const size_t n = write_rep_block(s, *state, shadow, w, opts);
    CHECK(n == b.bits && same_as(w, bytes, 0, b.bits), (name + ": the state writes back the exact bits").c_str());
    return state;
}

// ---- FQuat ---------------------------------------------------------------------

static bool same_quat(const Quat& a, double x, double y, double z) { return a == Quat{x, y, z}; }

static void test_quat() {
    std::printf("FQuat (client exe 0x141438440)\n");
    Value v;
    CHECK(slice_round_trip(capture_blocks::kQuatIdentity, "FQuat", &v), "the captured Rotation of a TargetTransforms element: 192 bits read and written back identically");
    const Quat* q = v.get_if<Quat>();
    CHECK(q && same_quat(*q, 0, 0, 0), "it is X = Y = Z = 0, the identity (W is derived, 1)");
    const auto full = quat_from_wire(q ? *q : Quat{});
    CHECK(full[0] == 0 && full[1] == 0 && full[2] == 0 && full[3] == 1, "quat_from_wire gives (0, 0, 0, 1)");

    const auto t = trip("FQuat", Value(Quat{0.5, -0.5, 0.25}));
    CHECK(t.whole && t.bits == 192 && t.value->get_if<Quat>() && same_quat(*t.value->get_if<Quat>(), 0.5, -0.5, 0.25),
          "a general value is three doubles: 192 bits, read back");
    CHECK(!struct_accepts("FQuat", Value(Vec3d{})), "a Vec3d is not a Quat");
// [C-O-A x Cyb]

    // The load: W = sqrt(1 - X^2 - Y^2 - Z^2); beyond the unit ball W = 0 and the vector is scaled to length 1.
    const auto half = quat_from_wire(Quat{0.5, -0.5, 0.5});
    CHECK(half[0] == 0.5 && half[1] == -0.5 && half[2] == 0.5 && half[3] == 0.5, "load: (0.5, -0.5, 0.5) has W = 0.5");
    const auto big = quat_from_wire(Quat{2, 0, 0});
    CHECK(big[0] == 1 && big[1] == 0 && big[2] == 0 && big[3] == 0, "load: X = 2 has W < 0 in the square: W = 0, X scaled to 1");

    // The save: identity for a null quaternion, scaled to unit length, W made non-negative.
    CHECK(same_quat(quat_to_wire(0, 0, 0, 0), 0, 0, 0), "save: the zero quaternion is sent as the identity");
    CHECK(same_quat(quat_to_wire(0, 0, 0, 1e-5), 0, 0, 0), "save: |q|^2 = 1e-10 (<= 1e-8) is the identity too");
    CHECK(same_quat(quat_to_wire(0.5, -0.5, 0.5, -0.5), -0.5, 0.5, -0.5), "save: a negative W flips all four signs");
    CHECK(same_quat(quat_to_wire(2, 0, 0, 0), 1, 0, 0) && same_quat(quat_to_wire(0, 0, 0, 2), 0, 0, 0),
          "save: |q|^2 = 4 is scaled by 1/sqrt(4) = 0.5");
    CHECK(same_quat(quat_to_wire(0.6, 0, 0.8, 0), 0.6, 0, 0.8), "save: a unit quaternion is left alone");
    CHECK(same_quat(quat_to_wire(0.6, 0, 0.8005, 0), 0.6, 0, 0.8005), "save: within 0.01 of unit (|1 - |q|^2| < 0.01) it is left alone too");

    // What the client reads is what was saved (a unit quaternion survives the round trip exactly).
    bool exact = true;
    const double samples[][3] = {{0.1, 0.2, 0.3}, {0.5, 0.5, 0.5}, {0.7, 0.1, -0.2}, {-0.3, 0.6, 0.1}};
    for (const auto& s : samples) {
        const auto f = quat_from_wire(Quat{s[0], s[1], s[2]});
        exact = exact && same_quat(quat_to_wire(f[0], f[1], f[2], f[3]), s[0], s[1], s[2]);
    }
    CHECK(exact, "quat_to_wire(quat_from_wire(q)) is q");

    BunchWriter w;
    write_struct("FQuat", Value(Quat{1, 2, 3}), w);
    PacketReader r(w.data(), w.byte_size(), 191);
    CHECK(!read_struct("FQuat", r).has_value(), "one bit short is not read");
}

// ---- FGuidToBoolWrapper ----------------------------------------------------------

static void test_guid_to_bool() {
    std::printf("FGuidToBoolWrapper (client exe 0x145a7d730)\n");
    Value one, two, many;
    CHECK(slice_round_trip(capture_blocks::kGuidToBoolOne, "FGuidToBoolWrapper", &one), "one GUID (192 bits) reads to the last bit and writes back identically");
    CHECK(slice_round_trip(capture_blocks::kGuidToBoolTwo, "FGuidToBoolWrapper", &two), "two GUIDs (352 bits)");
    CHECK(slice_round_trip(capture_blocks::kGuidToBoolMany, "FGuidToBoolWrapper", &many), "twenty GUIDs (3,232 bits)");
    const auto* w1 = one.get_if<GuidToBoolWrapper>();
    CHECK(w1 && w1->pairs.size() == 1 && w1->pairs[0].first == guid(3864118250u, 1327319643u, 3130987175u, 3686768017u) && w1->pairs[0].second,
          "the first is GUID (3864118250, 1327319643, 3130987175, 3686768017) -> true");
    const auto* w2 = two.get_if<GuidToBoolWrapper>();
    CHECK(w2 && w2->pairs.size() == 2 && w2->pairs[1].first == guid(2756160867u, 4239474767u, 2884361518u, 2797427392u) && w2->pairs[1].second,
          "the second holds two, in the map's order");
    const auto* wm = many.get_if<GuidToBoolWrapper>();
    CHECK(wm && wm->pairs.size() == 20, "and the third twenty");

    // Built values: the bool is an int32, false is 0.
    GuidToBoolWrapper built;
    built.pairs.emplace_back(guid(1, 2, 3, 4), false);
    built.pairs.emplace_back(guid(5, 6, 7, 8), true);
    const auto t = trip("FGuidToBoolWrapper", Value(built));
    CHECK(t.whole && t.bits == 32 + 2 * 160 && t.value->get_if<GuidToBoolWrapper>() && *t.value->get_if<GuidToBoolWrapper>() == built,
          "a map of two: int32 Num and 160 bits a pair (FGuid + int32 bool), false and true read back");
    const auto empty = trip("FGuidToBoolWrapper", Value(GuidToBoolWrapper{}));
    CHECK(empty.whole && empty.bits == 32, "an empty map is its count alone");

    // Refusals.
    GuidToBoolWrapper dup;
    dup.pairs.emplace_back(guid(1, 2, 3, 4), true);
    dup.pairs.emplace_back(guid(1, 2, 3, 4), false);
    BunchWriter out;
    CHECK(!struct_accepts("FGuidToBoolWrapper", Value(dup)) && !write_struct("FGuidToBoolWrapper", Value(dup), out) && out.bit_pos() == 0,
          "a repeated GUID (a TMap holds each key once) is not written");
    BunchWriter bad_bool;
    bad_bool.write_int32(1);
    for (uint32_t x : {1u, 2u, 3u, 4u}) bad_bool.write_uint32(x);
    bad_bool.write_uint32(2);
    CHECK(!read_bits_as("FGuidToBoolWrapper", bad_bool).has_value(), "a bool of 2 is not read (the archive's error)");
    BunchWriter neg;
    neg.write_int32(-1);
    CHECK(!read_bits_as("FGuidToBoolWrapper", neg).has_value(), "a negative count is not read");
    BunchWriter lie;
    lie.write_int32(3);
    for (int i = 0; i < 5; ++i) lie.write_uint32(0);
    CHECK(!read_bits_as("FGuidToBoolWrapper", lie).has_value(), "a count the rest of the input cannot hold is not read");
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kGuidToBoolTwo.hex);
    PacketReader shy(bytes.data(), bytes.size(), capture_blocks::kGuidToBoolTwo.bits - 1);
    CHECK(!read_struct("FGuidToBoolWrapper", shy).has_value(), "one bit short is not read");
}

/// The item layout of UInteractableComponent's InteractVisibleStateMap / InteractGameplayStateMap: Key and
/// the FGuidToBoolWrapper (ReplicationID / ReplicationKey / MostRecentArrayReplicationKey are RepSkip;
/// tools/ghidra/fast_array_check.py reads the captured payloads with the same two commands).
static void test_interactable_payload() {
    std::printf("InteractVisibleStateMap payload (fast array of Key + FGuidToBoolWrapper, layout built by hand)\n");
    using schema::PropType;
    using schema::RepCommand;
    static const RepCommand kItem[] = {
        {1, 0, "Items.Key", "UInteractableComponent", PropType::Int64, 0, 64, nullptr, nullptr, 0, 0},
        {2, 0, "Items.Value", "UInteractableComponent", PropType::NetSerializeStruct, 0, 0, "FGuidToBoolWrapper", nullptr, 0, 0},
    };
    const FastArrayLayout layout{9, std::span<const RepCommand>(kItem), std::span<const RepCommand>(kItem)};
    const Slice& s = capture_blocks::kInteractStates182;
    const auto bytes = capture_blocks::bytes_of(s.hex);
    size_t pos = 0;
    const auto delta = read_fast_array_delta(layout, bytes.data(), s.bits, pos);
    CHECK(delta && pos == s.bits, "the payload (577 bits) reads to the last bit");
    if (!delta) return;
    CHECK(delta->array_key == 649 && delta->base_key == 0 && delta->deleted.empty() && delta->changed.size() == 1 && delta->changed[0].id == 43,
          "key 649 against base 0, one item, ID 43");
    const auto& values = delta->changed[0].values.values;
    const auto* value = values.count(2) ? values.at(2).get_if<GuidToBoolWrapper>() : nullptr;
    CHECK(values.count(1) && values.at(1).get_if<int64_t>() && *values.at(1).get_if<int64_t>() == 0 && value && value->pairs.size() == 2,
          "Key 0 and a map of two GUIDs");
    FastArrayState state(layout);
    CHECK(apply_fast_array_delta(state, *delta), "applied to an empty array");
    FastArrayShadow shadow;
    BunchWriter w;
    bool failed = false;
    const size_t n = write_fast_array_delta(state, shadow, w, {}, &failed);
    CHECK(!failed && n == s.bits && same_as(w, bytes, 0, s.bits), "and written back to the exact bits (a fresh shadow)");
}

// ---- FAlignmentContainer ------------------------------------------------------------

static void test_alignment() {
    std::printf("FAlignmentContainer (client exe 0x145e77630)\n");
    Value v;
    CHECK(slice_round_trip(capture_blocks::kAlignmentContainerPawn, "FAlignmentContainer", &v),
          "the pawn's value (960 bits) reads to the last bit and writes back identically");
    const auto* c = v.get_if<AlignmentContainer>();
    CHECK(c && c->instances.size() == 4 && c->timers.empty(), "four instances, no timers");
    CHECK(c && c->instances[0].key == 6064629444695359488ull && c->instances[0].guids.size() == 1 && c->instances[0].guids[0] == guid(0, 0, 0, 0),
          "the first: key 6064629444695359488, the null GUID");
    CHECK(c && c->instances[2].key == 6064630243140894720ull && c->instances[2].guids.size() == 1 &&
              c->instances[2].guids[0] == guid(3635609113u, 2516274681u, 2602449240u, 1814768596u),
          "the third: key 6064630243140894720, GUID (3635609113, 2516274681, 2602449240, 1814768596)");

    // The whole blocks: the RepLayout block with handle 4 (Alignments).
    std::optional<RepState> npc_state;
    for (const Block* b : {&capture_blocks::kAlignmentNpc24, &capture_blocks::kAlignmentPawn107}) {
        const auto state = rep_block_round_trip(*b);
        const Value* a = state ? state->get(4) : nullptr;
        CHECK(a && a->get_if<AlignmentContainer>() && state->values().size() == 1, (std::string(b->name) + ": handle 4 (Alignments) is the only value").c_str());
        if (b == &capture_blocks::kAlignmentNpc24) npc_state = state;
    }
    {
        const auto* a = npc_state && npc_state->get(4) ? npc_state->get(4)->get_if<AlignmentContainer>() : nullptr;
        CHECK(a && a->instances.size() == 2 && a->instances[0].key == 6064629454767587359ull && a->instances[1].key == 6064630243140894720ull,
              "the NPC's has two instances");
    }

    // InstanceTimers: no captured container has one; the layout is the exe's.
    AlignmentContainer built;
    built.instances.push_back({5, {guid(1, 2, 3, 4), guid(5, 6, 7, 8)}});
    built.instances.push_back({6, {}});
    built.timers.push_back({7, std::bit_cast<uint64_t>(12.5), 0x1122334455667788ull, 0x99aabbccddeeff00ull});
    built.timers.push_back({8, 0, 1, 2});
    const auto t = trip("FAlignmentContainer", Value(built));
    const size_t expect = 32 + (64 + 32 + 2 * 128) + (64 + 32) + 32 + 2 * (32 + 3 * 64);
    CHECK(t.whole && t.bits == expect && t.value->get_if<AlignmentContainer>() && *t.value->get_if<AlignmentContainer>() == built,
          "instances with sets and two timers round trip: int32 counts, 8-byte keys, FGuids, 4-byte timer keys + three 8-byte words");
    const auto empty = trip("FAlignmentContainer", Value(AlignmentContainer{}));
    CHECK(empty.whole && empty.bits == 64, "an empty container is two counts: 64 bits");
// (C-Y-B | CoA)

    // Refusals.
    AlignmentContainer dup_instance = built;
    dup_instance.instances[1].key = 5;
    AlignmentContainer dup_guid = built;
    dup_guid.instances[0].guids[1] = guid(1, 2, 3, 4);
    AlignmentContainer dup_timer = built;
    dup_timer.timers[1].key = 7;
    BunchWriter out;
    CHECK(!struct_accepts("FAlignmentContainer", Value(dup_instance)) && !struct_accepts("FAlignmentContainer", Value(dup_guid)) &&
              !struct_accepts("FAlignmentContainer", Value(dup_timer)) && !write_struct("FAlignmentContainer", Value(dup_guid), out) && out.bit_pos() == 0,
          "a repeated instance key, GUID of a set or timer key is not written");
    CHECK(!struct_accepts("FAlignmentContainer", Value(Vec3d{})), "another value type is not accepted");
    BunchWriter neg;
    neg.write_int32(-3);
    CHECK(!read_bits_as("FAlignmentContainer", neg).has_value(), "a negative count is not read");
    BunchWriter lie;
    lie.write_int32(1);
    lie.write_uint64(9);
    lie.write_int32(1000);   // a set of a thousand GUIDs in a few bits
    lie.write_int32(0);
    CHECK(!read_bits_as("FAlignmentContainer", lie).has_value(), "a set larger than the input is not read");

    // RepState accepts it for the schema's command and refuses it elsewhere.
    const auto& align = schema_named("UAlignmentComponent");
    RepState st;
    CHECK(st.set(align, "Alignments", Value(built)) && st.set(align, "InheritedAlignments", Value(AlignmentContainer{})) &&
              !st.set(align, "bIsActive", Value(built)),
          "RepState::set takes an AlignmentContainer for Alignments and InheritedAlignments, not for a bool");
}

// ---- FAppearanceId --------------------------------------------------------------------

static void test_appearance_id() {
    std::printf("FAppearanceId (client exe 0x145919170)\n");
    Value v;
    CHECK(slice_round_trip(capture_blocks::kAppearanceIdPawn, "FAppearanceId", &v), "a captured value (144 bits) reads to the last bit and writes back identically");
    const auto* a = v.get_if<AppearanceId>();
    CHECK(a && a->record_guid == 6064630736269279236ull && a->type_id == 2616523457551378613ull && a->enchantment_level == 0 && a->item_equip_slot == 1,
          "RecordGuid 6064630736269279236, TypeId 2616523457551378613, EnchantmentLevel 0, ItemEquipSlot 1");

    BunchWriter w;
    write_struct("FAppearanceId", Value(AppearanceId{0x1111111111111111ull, 0x2222222222222222ull, 3, 200}), w);
    CHECK(w.bit_pos() == 144 && w.data()[0] == 0x22 && w.data()[8] == 0x11 && w.data()[16] == 3 && w.data()[17] == 200,
          "the wire order is TypeId, RecordGuid, EnchantmentLevel, ItemEquipSlot");

    const auto state = rep_block_round_trip(capture_blocks::kAppearanceIdsPawn107);
    const Value* ids = state ? state->get(3) : nullptr;
    const auto* arr = ids ? ids->get_if<ArrayValue>() : nullptr;
    CHECK(arr && arr->elements.size() == 4, "the pawn's AppearanceIDs (handle 3) is an array of four");
    bool typed = arr != nullptr;
    if (arr) {
        for (const ElementState& e : arr->elements) typed = typed && e.values.count(1) && e.values.at(1).get_if<AppearanceId>();
    }
    CHECK(typed, "each element is an AppearanceId");
    CHECK(!struct_accepts("FAppearanceId", Value(Quat{})), "another value type is not accepted");
}

// ---- FCharacterCustomizationSaveData ----------------------------------------------------

static const CharacterCustomization* customization_of(const Value& v) { return v.get_if<CharacterCustomization>(); }

static CharacterCustomization full_customization() {
    CharacterCustomization c;
    c.preset_guid = 0x0102030405060708ull;
    c.random_seed = 1454;
    c.race_gender = CharacterCustomization::RaceGender{2, 1};
    for (size_t i = 0; i < kCustomizationFieldCount; ++i) {
        const uint8_t bits = (i < 5 || (i >= 12 && i < 15) || (i >= 18 && i < 22) || i == 24 || i == 25 || i == 27 || i == 29) ? 32 : 64;
        c.deltas[static_cast<CustomizationField>(i)] = bits == 32 ? (0x40000000u + static_cast<uint32_t>(i)) : (0x8000000000000000ull | i);
    }
    CustomizationDecal d;
    d.guid = 11; d.strength = 0x3f800000; d.scale_u = 2; d.scale_v = 3; d.slot_index = 4; d.merge_section_material_base = 5;
    d.offset_u = 6; d.offset_v = 7; d.angle = 8; d.color = 9; d.discoloration = 10;
    c.decals = std::vector<std::optional<CustomizationDecal>>{std::nullopt, d};
    c.blend_groups = std::vector<std::optional<CustomizationBlendGroup>>{CustomizationBlendGroup{1, 2, 3}, std::nullopt};
    c.helmet_visible = false;
    c.cape_visible = true;
    c.sections_values = std::vector<CustomizationSection>{{0, {0.25, 0.5, 0.75}}, {7, {-1.0, 0.0, 1e10}}};
    c.face_morph_weights = std::vector<CustomizationMorph>{{NameValue::hardcoded_name(300), 0x3f000000},
                                                           {NameValue::plain("browRaise", 0), 0x3e800000},
                                                           {NameValue::plain("jawWide", 3), 0}};
    return c;
}

static void test_customization() {
    std::printf("FCharacterCustomizationSaveData (client exe 0x1463d6290)\n");

    // The pawn's whole Character Appearance block: SharedAppearanceInfoId (handle 4) and CharacterCustomization (7).
    {
        const auto state = rep_block_round_trip(capture_blocks::kCustomizationPawn78);
        const Value* cv = state ? state->get(7) : nullptr;
        const CharacterCustomization* c = cv ? customization_of(*cv) : nullptr;
        CHECK(c && state->get(4) && state->values().size() == 2, "handles 4 (SharedAppearanceInfoId.Guid) and 7 (CharacterCustomization)");
        CHECK(c && !c->preset_guid && c->random_seed && *c->random_seed == 1454 && c->race_gender && c->race_gender->race == 0 && c->race_gender->gender == 0,
              "no PresetGuid, RandomSeed 1454, Race 0 and Gender 0 (the client knows no preset: the bytes are on the wire)");
        CHECK(c && c->deltas.size() == 20 && c->deltas.count(CustomizationField::SkinColorHue) &&
                  c->deltas.at(CustomizationField::SkinColorHue) == 0x3f682694u && c->deltas.at(CustomizationField::SkinSet) == 2 &&
                  c->deltas.at(CustomizationField::EyeColor) == 108238840485496ull && !c->deltas.count(CustomizationField::NailColor),
              "20 scalar deltas: SkinColorHue 0x3f682694 (a float), SkinSet 2, EyeColor 108238840485496 ..., no NailColor");
        CHECK(c && c->decals && c->decals->size() == 2 && (*c->decals)[0] && (*c->decals)[1] && (*c->decals)[0]->guid == 6064631051941838856ull &&
                  (*c->decals)[0]->strength == 1065353216 && (*c->decals)[1]->slot_index == 1,
              "DecalData: two decals sent whole (Strength 1.0f, the second SlotIndex 1)");
        CHECK(c && c->blend_groups && c->blend_groups->size() == 2 && !(*c->blend_groups)[0] && (*c->blend_groups)[1] &&
                  (*c->blend_groups)[1]->decal_appearance_guid == 6064631035312930818ull && (*c->blend_groups)[1]->color_guid == 0,
              "DecalBlendGroups: the first unchanged, the second sent");
        CHECK(c && c->sections_values && c->sections_values->size() == 30 && (*c->sections_values)[0].key == 0 &&
                  ((*c->sections_values)[0].value == Vec3d{0.6896370633481491, 0.3065618833408993, 0.3718062632874055}) && !c->helmet_visible && !c->cape_visible,
              "SectionsValues: 30 sections, the first an FVector of three doubles; the two visibility bools are not sent");
        CHECK(c && c->face_morph_weights && c->face_morph_weights->size() == 83 && (*c->face_morph_weights)[0].name == NameValue::plain("Nose_Bridge_Hooked", 1) &&
                  (*c->face_morph_weights)[0].weight == 0 && (*c->face_morph_weights)[1].name == NameValue::plain("Temples_Sharp", 1) &&
                  (*c->face_morph_weights)[1].weight == 0x3f800000,
              "FaceMorphWeightMaps: 83 morph targets by FName (Nose_Bridge_Hooked_0 = 'Nose_Bridge_Hooked' number 1: 0.0f, Temples_Sharp_0: 1.0f ...)");
    }

    // Values.
    Value npc24, npc66, pawn;
    CHECK(slice_round_trip(capture_blocks::kCustomizationNpc24, "FCharacterCustomizationSaveData", &npc24), "an NPC's value (1,054 bits) reads to the last bit and writes back identically");
    CHECK(slice_round_trip(capture_blocks::kCustomizationNpc66, "FCharacterCustomizationSaveData", &npc66), "another NPC's (406 bits)");
    CHECK(slice_round_trip(capture_blocks::kCustomizationValuePawn78, "FCharacterCustomizationSaveData", &pawn), "the pawn's (26,861 bits)");
    const auto* n24 = customization_of(npc24);
    CHECK(n24 && n24->random_seed && *n24->random_seed == 407783230u && n24->race_gender && n24->race_gender->race == 0 && n24->race_gender->gender == 1 &&
              n24->deltas.count(CustomizationField::EyeColor) && n24->deltas.at(CustomizationField::EyeColor) == 6064631913016262657ull,
          "the first NPC: RandomSeed 407783230, Race 0, Gender 1, EyeColor 6064631913016262657");
    const auto* n66 = customization_of(npc66);
    CHECK(n66 && n66->random_seed && *n66->random_seed == 947559098u && n66->race_gender && n66->race_gender->gender == 0,
          "the second NPC: RandomSeed 947559098, Gender 0");

    // Every field kind.
    const CharacterCustomization full = full_customization();
    const auto t = trip("FCharacterCustomizationSaveData", Value(full));
    CHECK(t.whole && t.value->get_if<CharacterCustomization>() && *t.value->get_if<CharacterCustomization>() == full,
          "a value with every field kind (all 30 deltas, decals, blend groups, both bools, sections, hardcoded and plain morph names) round trips");
    CharacterCustomization bare;
    bare.race_gender = CharacterCustomization::RaceGender{};
    const auto tb = trip("FCharacterCustomizationSaveData", Value(bare));
    CHECK(tb.whole && tb.bits == 1 + 1 + 16 + 30 + 1 + 1 + 2 + 1 + 1,
          "nothing sent but Race and Gender: 54 bits (the flag bits of every field)");
    CodecOptions any_known;
    any_known.customization_preset_known = [](uint64_t) { return true; };
    const auto tn = trip("FCharacterCustomizationSaveData", Value(CharacterCustomization{}), any_known);
    CHECK(tn.whole && tn.bits == 38, "with no Race and Gender (a preset the client knows): 38 bits");

    // The preset is not on the wire.
    CharacterCustomization preset;
    preset.preset_guid = 42;
    preset.deltas[CustomizationField::SkinSet] = 3;
    CodecOptions known;
    known.customization_preset_known = [](uint64_t g) { return g == 42; };
    const auto tk = trip("FCharacterCustomizationSaveData", Value(preset), known);
    CHECK(tk.whole && tk.bits == 1 + 64 + 1 + 30 + 32 + 1 + 1 + 2 + 1 + 1 && *tk.value->get_if<CharacterCustomization>() == preset && !tk.value->get_if<CharacterCustomization>()->race_gender,
          "a known preset: no Race and Gender bytes on the wire, and the reader is told by CodecOptions::customization_preset_known");
    BunchWriter w;
    write_struct("FCharacterCustomizationSaveData", Value(preset), w, known);
    const auto misread = read_bits_as("FCharacterCustomizationSaveData", w);   // default options: no preset is known
    CHECK(!misread || !(*misread->get_if<CharacterCustomization>() == preset),
          "the same bits read with no known preset are not that value: the wire alone does not say");

    // Refusals.
    CharacterCustomization wide = bare;
    wide.deltas[CustomizationField::SkinSet] = 0x100000000ull;
    CharacterCustomization dup_morph = bare;
    dup_morph.face_morph_weights = std::vector<CustomizationMorph>{{NameValue::plain("a"), 1}, {NameValue::plain("a"), 2}};
    CharacterCustomization bad_name = bare;
    bad_name.face_morph_weights = std::vector<CustomizationMorph>{{NameValue::plain("\xff"), 1}};
    CharacterCustomization dup_section = bare;
    dup_section.sections_values = std::vector<CustomizationSection>{{1, {}}, {1, {}}};
    BunchWriter out;
    CHECK(!struct_accepts("FCharacterCustomizationSaveData", Value(wide)) && !struct_accepts("FCharacterCustomizationSaveData", Value(dup_morph)) &&
              !struct_accepts("FCharacterCustomizationSaveData", Value(bad_name)) && !struct_accepts("FCharacterCustomizationSaveData", Value(dup_section)) &&
              !write_struct("FCharacterCustomizationSaveData", Value(wide), out) && out.bit_pos() == 0,
          "a delta wider than its field, a repeated morph name or section key and a name that is not UTF-8 are not written");
    CHECK(!struct_accepts("FCharacterCustomizationSaveData", Value(AppearanceId{})), "another value type is not accepted");
// C-Y-B - C O A

    BunchWriter bad_bool;   // preset bit 0, seed bit 0, Race and Gender, 30 clear deltas, decals 0, blend 0, helmet: set, 2
    bad_bool.write_bits(0, 2);
    bad_bool.write_bits(0, 16);
    bad_bool.write_bits(0, 30);
    bad_bool.write_bits(0, 2);
    bad_bool.write_bit(1);
    bad_bool.write_uint32(2);
    CHECK(!read_bits_as("FCharacterCustomizationSaveData", bad_bool).has_value(), "a visibility bool of 2 is not read");
    BunchWriter lie;        // a DecalData array of a million elements in a few bits
    lie.write_bits(0, 2);
    lie.write_bits(0, 16);
    lie.write_bits(0, 30);
    lie.write_bit(1);
    lie.write_int32(1000000);
    CHECK(!read_bits_as("FCharacterCustomizationSaveData", lie).has_value(), "an array count the input cannot hold is not read");
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kCustomizationNpc66.hex);
    PacketReader shy(bytes.data(), bytes.size(), capture_blocks::kCustomizationNpc66.bits - 1);
    CHECK(!read_struct("FCharacterCustomizationSaveData", shy).has_value(), "one bit short is not read");

    // RepState accepts it for the schema's command.
    const auto& appearance = schema_named("UCharacterAppearanceComponent");
    RepState st;
    CHECK(st.set(appearance, "CharacterCustomization", Value(full)) && !st.set(appearance, "CharacterCustomization", Value(AppearanceId{})),
          "RepState::set takes a CharacterCustomization for CharacterCustomization, nothing else");
}

// ---- the ability component's fast array: FQuat inside an item ------------------------------

static const Quat* rotation_of_item(const FastArrayDelta::Changed& c) {
    const auto it = c.values.values.find(4);   // TargetTransforms
    if (it == c.values.values.end()) return nullptr;
    const ArrayValue* transforms = it->second.get_if<ArrayValue>();
    if (!transforms || transforms->elements.empty()) return nullptr;
    const auto rot = transforms->elements[0].values.find(1);   // Rotation
    return rot == transforms->elements[0].values.end() ? nullptr : rot->second.get_if<Quat>();
}

static void test_ability_payloads() {
    std::printf("ReplicatedActiveAbilities: FQuat inside an item's TargetTransforms\n");
    const auto& s = schema_named("UAoCAbilityComponent");

    // An open: the RepLayout block, then field 4.
    {
        const Block& b = capture_blocks::kAbilityOpen62;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        size_t pos = 0;
        const auto state = read_rep_block(s, bytes.data(), b.bits, pos);
        CHECK(state && pos == b.rep_bits, "the open's RepLayout block (202 bits) reads");
        const auto fields = read_fast_array_fields(s, bytes.data(), b.bits, pos);
        CHECK(fields && pos == b.bits && fields->size() == 1 && (*fields)[0].field == 4,
              "and the field loop reads to the last bit: field 4, ReplicatedActiveAbilities");
        if (!state || !fields) return;
        const FastArrayDelta& d = (*fields)[0].delta;
        CHECK(d.array_key == 4 && d.base_key == 0 && d.changed.size() == 1 && d.changed[0].id == 1 && d.deleted.empty(),
              "key 4 against base 0, one item, ID 1");
        const Quat* rot = d.changed.empty() ? nullptr : rotation_of_item(d.changed[0]);
        CHECK(rot && same_quat(*rot, 0, 0, 0), "its TargetTransforms element has the identity Rotation");
        const auto tr = d.changed[0].values.values.at(4).get_if<ArrayValue>()->elements[0].values.at(2).get_if<Vec3d>();
        CHECK(tr && tr->x == -771967.75 && tr->z == 15986.45, "and its Translation is the FVector (-771967.75, 627540.48, 15986.45)");

        FastArrays arrays;
        FastArrayState* st = arrays.get(s, 4);
        CHECK(st && apply_fast_array_delta(*st, d), "the array applied");
        BunchWriter w;
        RepShadow shadow;
        FastArrayShadows shadows;
        bool failed = false;
        const size_t rep = write_rep_block(s, *state, shadow, w);
        const size_t rest = write_fast_array_fields(s, arrays, shadows, w, {}, &failed);
        CHECK(!failed && rep == b.rep_bits && rest == b.bits - b.rep_bits && same_as(w, bytes, 0, b.bits),
              "the RepLayout block then the field loop write back the exact 1,884 bits");
    }

    // An update: the field loop alone, base key 97672.
    {
        const Block& b = capture_blocks::kAbilityUpdate1895;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        size_t pos = 0;
        const auto fields = read_fast_array_fields(s, bytes.data(), b.bits, pos);
        CHECK(fields && pos == b.bits && fields->size() == 1 && (*fields)[0].field == 4, "an update (1,554 bits) reads to the last bit");
        if (!fields) return;
        const FastArrayDelta& d = (*fields)[0].delta;
        CHECK(d.array_key == 97674 && d.base_key == 97672 && d.changed.size() == 1 && d.changed[0].id == 19561,
              "key 97674 against base 97672, item 19561");
        const Quat* rot = d.changed.empty() ? nullptr : rotation_of_item(d.changed[0]);
        CHECK(rot && same_quat(*rot, 0, 0, 0), "with the identity Rotation");
        const auto layout = fast_array_layout(s, 4);
        if (!layout) return;
        FastArrayState st(*layout);
        FastArrayShadow shadow;
        st.set_array_key(d.array_key);
        CHECK(st.insert(d.changed[0].id, d.changed[0].values), "the item inserted into an array of key 97674");
        shadow.array_key = d.base_key;   // the connection had the array as it was before this one item
        BunchWriter w;
        bool failed = false;
        const size_t n = write_fast_array_delta(st, shadow, w, {}, &failed);
        // write_fast_array_delta writes the payload alone; the block's first 21 bits are the field header
        CHECK(!failed && n == b.bits - 21 && same_as(w, bytes, 21, n), "the payload writes back the exact bits against a shadow at the base key");
    }
}

int main() {
    test_quat();
    test_guid_to_bool();
    test_interactable_payload();
    test_alignment();
    test_appearance_id();
    test_customization();
    test_ability_payloads();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
