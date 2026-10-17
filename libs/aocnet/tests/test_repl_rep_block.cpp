// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_rep_block.cpp
//
//  RepState, RepShadow and the RepLayout block (protocol/repl/) against the
//  blocks the original server sent (capture_blocks.h, fixtures/replay_data.bin):
//    - every captured block reads to its last bit (read_rep_block), and the
//      state it gives writes back the exact bits (write_rep_block into an empty
//      shadow): PlayerController open, CharacterLoadTracker updates, settings and
//      attachment rotation (ch 3 pkt#22, #46, #72, #620, #7706), pawn open
//      (ch 19 pkt#78, FRepMovement), the pawn's possession, unhide and view pitch
//      updates (pkt#107, #2523, #7706), the PlayerState open and attachment (ch 193
//      pkt#434, #491), and pawn / NPC component blocks (UPlayerInfo,
//      UPlayerCombatInfo, UOwnerInfoComponent, UAoCAbilityComponent with arrays of
//      57 and 58 elements);
//    - the same small blocks built from scratch with RepState::set;
//    - the shadow: no change writes nothing, one change writes that handle;
//    - the field loop header that follows a block (UPlayerInfo HeldItems);
//    - hostile input: array counts (a bounded number of elements however large they say),
//      element handles that do not fit or do not ascend;
//    - what RepState::set accepts is what the writer writes (quantized components, ids,
//      strings), an element value that is gone is sent as its zero, the parent mask.
// ============================================================================
#include "capture_blocks.h"
#include "protocol/repl/rep_block.h"
#include "protocol/schema/generated/index.h"

#include <bit>
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

static const schema::ClassSchema& schema_of(const Block& b) {
    const schema::ClassSchema* s = gen::find_class_schema(b.class_name);
    if (!s) { std::printf("  [FAIL] no schema %s\n", b.class_name); std::exit(2); }
    return *s;
}

/// The bytes of a block and its decoded state.
struct Decoded {
    std::vector<uint8_t> bytes;
    std::optional<RepState> state;
    size_t pos = 0;
};
// C:Y:B x C O A

static Decoded decode(const Block& b, const CodecOptions& opts = {}) {
    Decoded d;
    d.bytes = capture_blocks::bytes_of(b.hex);
    d.state = read_rep_block(schema_of(b), d.bytes.data(), b.bits, d.pos, opts);
    return d;
}

/// True when writing `state` into an empty shadow gives the first `bits` bits of `expect`.
static bool writes_exactly(const schema::ClassSchema& s, const RepState& state, const uint8_t* expect, size_t bits,
                           const CodecOptions& opts = {}) {
    BunchWriter w;
    RepShadow shadow;
    const size_t n = write_rep_block(s, state, shadow, w, opts);
    size_t diff = 0;
    if (n != bits || w.bit_pos() != bits) {
        std::printf("    wrote %zu bits, expected %zu\n", n, bits);
        return false;
    }
    if (!capture_blocks::same_bit_range(w.data(), expect, bits, &diff)) {
        std::printf("    first differing bit %zu\n", diff);
        return false;
    }
    return true;
}

/// `n` bits of `bytes` from bit `start`, as a buffer starting at bit 0.
static std::vector<uint8_t> slice_bits(const std::vector<uint8_t>& bytes, size_t start, size_t n) {
    std::vector<uint8_t> out((n + 7) / 8, 0);
    for (size_t i = 0; i < n; ++i) out[i >> 3] |= ((bytes[(start + i) >> 3] >> ((start + i) & 7)) & 1) << (i & 7);
    return out;
}

static emit::FIntrepidNetworkGUID guid(uint64_t id) {
    emit::FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = 60;
    g.Randomizer = 0x6ee87ee4;
    return g;
}

static void round_trip_block(const Block& b, const CodecOptions& opts = {}) {
    const std::string name = std::string(b.name) + " (" + b.class_name + ", " + std::to_string(b.rep_bits) + " bits)";
    const Decoded d = decode(b, opts);
    CHECK(d.state && d.pos == b.rep_bits, (name + ": reads to the last bit of the RepLayout block").c_str());
    if (!d.state) return;
    CHECK(writes_exactly(schema_of(b), *d.state, d.bytes.data(), b.rep_bits, opts),
          (name + ": the decoded state writes back the exact bits").c_str());
    BunchWriter w;
    RepShadow shadow;
    write_rep_block(schema_of(b), *d.state, shadow, w, opts);
    const size_t before = w.bit_pos();
    CHECK(write_rep_block(schema_of(b), *d.state, shadow, w, opts) == 0 && w.bit_pos() == before,
          (name + ": a second write of the same state writes nothing").c_str());
}

static void test_captured_blocks() {
    std::printf("captured blocks: read to the last bit, written back bit for bit\n");
    round_trip_block(capture_blocks::kPcOpenProps);
    round_trip_block(capture_blocks::kPcLoadTracker46);
    round_trip_block(capture_blocks::kPcLoadTracker72);
    round_trip_block(capture_blocks::kPcSettings620);
    round_trip_block(capture_blocks::kPcAttachRotation7706);
    round_trip_block(capture_blocks::kPawnOpenActor);
    round_trip_block(capture_blocks::kPawnPossessed107);
    round_trip_block(capture_blocks::kPawnUnhide2523);
    round_trip_block(capture_blocks::kPawnViewPitch7706);
    round_trip_block(capture_blocks::kPlayerStateOpen434);
    round_trip_block(capture_blocks::kPlayerStateAttach491);
    round_trip_block(capture_blocks::kPlayerInfoOpen);
    round_trip_block(capture_blocks::kPlayerCombatInfoOpen);
    round_trip_block(capture_blocks::kPlayerInfoPossessed);
    round_trip_block(capture_blocks::kOwnerInfo107);
    round_trip_block(capture_blocks::kAbilityPawn107);
    round_trip_block(capture_blocks::kAbilityNpc);
}

static void test_decoded_values() {
    std::printf("what the captured blocks decode to\n");
    {
        const Block& b = capture_blocks::kPcOpenProps;
        const Decoded d = decode(b);
        const auto& s = schema_of(b);
        const RepState& st = *d.state;
        CHECK(st.values().size() == 7, "PC open: 7 handles (1, 8, 16, 19, 22, 24, 52)");
        CHECK(st.get(s, "RemoteRole") && *st.get(s, "RemoteRole") == Value::u64(2) && st.get(s, "Role") && *st.get(s, "Role") == Value::u64(4),
              "RemoteRole 2 (AutonomousProxy on the wire), Role 4");
        const Value* ps = st.get(s, "PlayerState");
        const auto* g = ps ? ps->get_if<emit::FIntrepidNetworkGUID>() : nullptr;
        CHECK(g && g->ObjectId == 10341528 && g->ServerId == 60 && g->Randomizer == 0x6ee87ee4, "PlayerState is object 10341528 (server 60)");
        const Value* loc = st.get(s, "SpawnLocation");
        CHECK(loc && loc->get_if<Vec3d>() && loc->get_if<Vec3d>()->x == -777664.5, "SpawnLocation is an FVector");
        CHECK(st.get(s, "SummonCooldownTimer") && st.get(s, "SummonCooldownTimer")->get_if<float>(), "SummonCooldownTimer is a float");
    }
    {
        const Block& b = capture_blocks::kPawnOpenActor;
        const Decoded d = decode(b);
        const auto& s = schema_of(b);
        const Value* m = d.state->get(s, "ReplicatedMovement");
        const RepMovement* rm = m ? m->get_if<RepMovement>() : nullptr;
        CHECK(rm && rm->location.x == -77766450 && rm->rotation.yaw == 65, "pawn open: ReplicatedMovement decoded (byte rotation)");
        const Value* o = d.state->get(s, "Owner");
        CHECK(o && o->get_if<emit::FIntrepidNetworkGUID>() && o->get_if<emit::FIntrepidNetworkGUID>()->ObjectId == 10341530, "Owner is the PlayerController 10341530");
    }
    {
        const Block& b = capture_blocks::kPlayerInfoOpen;
        const Decoded d = decode(b);
        const auto& s = schema_of(b);
        const Value* n = d.state->get(s, "CharacterName");
        CHECK(n && n->get_if<std::string>() && *n->get_if<std::string>() == "RandomChar", "UPlayerInfo: CharacterName RandomChar");
        const Value* a = d.state->get(s, "CharacterGuid.A");
        CHECK(a && a->get_if<int64_t>() && static_cast<uint32_t>(*a->get_if<int64_t>()) == 3635609113u, "CharacterGuid.A is an Int32 (sign extended)");
        const Value* t = d.state->get(s, "CurrentBreathingTime");
        CHECK(t && t->get_if<float>() && *t->get_if<float>() == 30.0f, "CurrentBreathingTime 30");
    }
    {
        const Block& b = capture_blocks::kPlayerInfoPossessed;
        const Decoded d = decode(b);
        const auto& s = schema_of(b);
        const Value* w = d.state->get(s, "WorldAttunements.AttunedBlocks");
        const ArrayValue* a = w ? w->get_if<ArrayValue>() : nullptr;
        const Value* inner = a && a->elements.size() == 1 && !a->elements[0].values.empty() ? &a->elements[0].values.begin()->second : nullptr;
        const ArrayValue* ia = inner ? inner->get_if<ArrayValue>() : nullptr;
        CHECK(ia && ia->elements.size() == 1 && ia->elements[0].values.size() == 1 && *ia->elements[0].values.at(1).get_if<uint64_t>() == 1,
              "UPlayerInfo after possession: WorldAttunements is an array of one element holding an array of one value (1)");
        CHECK(!d.state->get(s, "HeldItems.Items"), "the HeldItems fast array is not in the RepLayout block");
    }
    {
        const Block& b = capture_blocks::kOwnerInfo107;
        const Decoded d = decode(b);
        const auto& s = schema_of(b);
        const Value* r = d.state->get(s, "ResourceContainers");
        const ArrayValue* a = r ? r->get_if<ArrayValue>() : nullptr;
        CHECK(a && a->elements.size() == 3 && a->elements[0].values.size() == 1 && a->elements[1].values.empty() && a->elements[2].values.empty(),
              "OwnerInfo: ResourceContainers has 3 elements and the server sent element 0 only");
        const Value* n = d.state->get(s, "WeaponSkillLineId.Name");
        CHECK(n && n->get_if<NameValue>() && n->get_if<NameValue>()->text == "Weapon_1H_Scepter", "WeaponSkillLineId.Name is an FName");
    }
    {
        const Block& b = capture_blocks::kAbilityPawn107;
        const Decoded d = decode(b);
        const auto& s = schema_of(b);
        const ArrayValue* known = d.state->get(s, "KnownAbilities") ? d.state->get(s, "KnownAbilities")->get_if<ArrayValue>() : nullptr;
        const ArrayValue* recs = d.state->get(s, "ReplicatedRecordsToLoad") ? d.state->get(s, "ReplicatedRecordsToLoad")->get_if<ArrayValue>() : nullptr;
        CHECK(known && known->elements.size() == 57 && recs && recs->elements.size() == 58, "AbilityComponent: arrays of 57 and 58 elements");
        const Value* nm = recs && recs->elements[0].values.count(3) ? &recs->elements[0].values.at(3) : nullptr;
        CHECK(nm && nm->get_if<NameValue>() && nm->get_if<NameValue>()->hardcoded && nm->get_if<NameValue>()->index == 0,
              "a record's Name is the hardcoded FName None (EName 0)");
    }
}
// ~ C | O | A - C | Y | B ~

/// The PlayerController's open block built with set().
static void test_built_from_scratch() {
    std::printf("blocks built from scratch with RepState::set\n");
    {
        const Block& b = capture_blocks::kPawnPossessed107;
        const auto& s = schema_of(b);
        RepState st;
        bool ok = st.set(s, "AuthServerIDReplicated", Value::u64(0));
        ok = st.set(s, "bHidden", Value::boolean(true)) && ok;
        ok = st.set(s, "RemoteRole", Value::u64(2)) && ok;
        ok = st.set(s, "Instigator", Value(guid(10341538))) && ok;
        ok = st.set(s, "PlayerState", Value(guid(10341528))) && ok;
        ok = st.set(s, "Controller", Value(guid(10341530))) && ok;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        CHECK(ok && writes_exactly(s, st, bytes.data(), 477), "pawn pkt#107 (possession): 477 bits");
    }
    {
        const Block& b = capture_blocks::kPcOpenProps;
        const auto& s = schema_of(b);
        RepState st;
        bool ok = st.set(s, "AuthServerIDReplicated", Value::u64(0));
        ok = st.set(s, "RemoteRole", Value::u64(2)) && ok;
        ok = st.set(s, "Role", Value::u64(4)) && ok;
        ok = st.set(s, "PlayerState", Value(guid(10341528))) && ok;
        ok = st.set(s, "SpawnLocation", Value(Vec3d{-777664.5, 616578.6875, 15952.0498046875})) && ok;
        ok = st.set(s, "CaravanLaunchNode", Value::i64(0)) && ok;
        ok = st.set(s, "SummonCooldownTimer", Value::f32(1593462.375f)) && ok;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        CHECK(ok && writes_exactly(s, st, bytes.data(), 519), "PC open pkt#22: 519 bits");
    }
    for (const Block* b : {&capture_blocks::kPcLoadTracker46, &capture_blocks::kPcLoadTracker72}) {
        const auto& s = schema_of(*b);
        LoadTracker lt;
        lt.flags.assign(25, b == &capture_blocks::kPcLoadTracker72);
        lt.flags[0] = true;
        if (b == &capture_blocks::kPcLoadTracker72) lt.flags[1] = false;
        RepState st;
        const bool ok = st.set(s, "CharacterLoadTracker", Value(lt));
        const auto bytes = capture_blocks::bytes_of(b->hex);
        CHECK(ok && writes_exactly(s, st, bytes.data(), 823), b == &capture_blocks::kPcLoadTracker72 ? "PC pkt#72 CharacterLoadTracker: 823 bits" : "PC pkt#46 CharacterLoadTracker: 823 bits");
    }
    {
        const Block& b = capture_blocks::kPawnOpenActor;
        const auto& s = schema_of(b);
        RepMovement m;
        m.location = QuantVec{QuantVec::Mode::Scaled, -77766450, 61657869, 1595205};
        m.rotation = Rot{0, 65, 0};
        m.linear_velocity = quantize_vector(1, 0, 0, 0);
        RepState st;
        bool ok = st.set(s, "AuthServerIDReplicated", Value::u64(0));
        ok = st.set(s, "ReplicatedMovement", Value(m)) && ok;
        ok = st.set(s, "RemoteRole", Value::u64(1)) && ok;
        ok = st.set(s, "Owner", Value(guid(10341530))) && ok;
        ok = st.set(s, "Role", Value::u64(4)) && ok;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        CHECK(ok && writes_exactly(s, st, bytes.data(), 332), "pawn open pkt#78 with FRepMovement: 332 bits");
    }
    {
        const Block& b = capture_blocks::kPlayerStateOpen434;
        const auto& s = schema_of(b);
        UniqueNetId id;
        id.flags = 8;
        id.id = "DESKTOP-EXAMPLE1-US-45739BC044607BD9A50196AF41F22492";
        RepState st;
        bool ok = st.set(s, "AuthServerIDReplicated", Value::u64(0));
        ok = st.set(s, "RemoteRole", Value::u64(1)) && ok;
        ok = st.set(s, "Owner", Value(guid(10341530))) && ok;
        ok = st.set(s, "Role", Value::u64(4)) && ok;
        ok = st.set(s, "PlayerId", Value::i64(1874)) && ok;
        ok = st.set(s, "StartTime", Value::i64(1593462)) && ok;
        ok = st.set(s, "UniqueId", Value(id)) && ok;
        ok = st.set(s, "PlayerNamePrivate", Value::str("DESKTOP-EXAMPLE1-US-")) && ok;
        const auto bytes = capture_blocks::bytes_of(b.hex);
        CHECK(ok && writes_exactly(s, st, bytes.data(), 967), "PlayerState open pkt#434 with FUniqueNetIdRepl and strings: 967 bits");
    }
}
// built by Cyb with C.O.A

static void test_shadow() {
    std::printf("the shadow: what the client already has is not sent again\n");
    const auto& s = schema_of(capture_blocks::kPawnPossessed107);
    RepState st;
    st.set(s, "AuthServerIDReplicated", Value::u64(0));
    st.set(s, "bHidden", Value::boolean(true));
    st.set(s, "RemoteRole", Value::u64(2));
    st.set(s, "Instigator", Value(guid(10341538)));
    st.set(s, "PlayerState", Value(guid(10341528)));
    st.set(s, "Controller", Value(guid(10341530)));

    BunchWriter w;
    RepShadow shadow;
    const size_t first = write_rep_block(s, st, shadow, w);
    CHECK(first == 477 && shadow.sent.size() == 6, "the first send writes every set handle (477 bits) and records them in the shadow");
    CHECK(write_rep_block(s, st, shadow, w) == 0 && w.bit_pos() == 477, "a second write with no change writes nothing");

    // pkt#2523: bHidden cleared: exactly that handle.
    st.set(s, "bHidden", Value::boolean(false));
    BunchWriter unhide;
    const size_t n = write_rep_block(s, st, shadow, unhide);
    const auto pkt2523 = capture_blocks::bytes_of(capture_blocks::kPawnUnhide2523.hex);
    CHECK(n == 18 && capture_blocks::same_bit_range(unhide.data(), pkt2523.data(), 18, nullptr),
          "changing bHidden writes that handle alone: the 18 bits of pkt#2523");
    CHECK(write_rep_block(s, st, shadow, unhide) == 0, "and then nothing more");

    // pkt#7706: RemoteViewPitch16 set later.
    st.set(s, "RemoteViewPitch16", Value::u64(61115));
    BunchWriter pitch;
    const size_t m = write_rep_block(s, st, shadow, pitch);
    const auto pkt7706 = capture_blocks::bytes_of(capture_blocks::kPawnViewPitch7706.hex);
    CHECK(m == 33 && capture_blocks::same_bit_range(pitch.data(), pkt7706.data(), 33, nullptr),
          "a value set later is written alone: the 33 bits of pkt#7706");

    // Setting the same value again is not a change; a new connection starts empty.
    st.set(s, "RemoteViewPitch16", Value::u64(61115));
    BunchWriter again;
    CHECK(write_rep_block(s, st, shadow, again) == 0 && again.bit_pos() == 0, "setting the same value again writes nothing");
    RepShadow other;
    BunchWriter fresh;
    CHECK(write_rep_block(s, st, other, fresh) > 33 && other.sent.size() == 7, "another connection's empty shadow gets every value");
    RepState none;
    RepShadow empty;
    BunchWriter nothing;
    CHECK(write_rep_block(s, none, empty, nothing) == 0 && nothing.bit_pos() == 0, "an empty state writes nothing");

    // Float and double changes compare by bit pattern.
    const auto& pc = schema_of(capture_blocks::kPcOpenProps);
    RepState f;
    f.set(pc, "SummonCooldownTimer", Value::f32(0.0f));
    RepShadow fs;
    BunchWriter fw;
    write_rep_block(pc, f, fs, fw);
    f.set(pc, "SummonCooldownTimer", Value::f32(-0.0f));
    CHECK(write_rep_block(pc, f, fs, fw) > 0, "-0.0 differs from 0.0: other bits go on the wire");
}

static void test_array_diff() {
    std::printf("dynamic arrays in the shadow\n");
    const Block& b = capture_blocks::kOwnerInfo107;
    const auto& s = schema_of(b);
    Decoded d = decode(b);
    RepShadow shadow;
    BunchWriter w;
    write_rep_block(s, *d.state, shadow, w);

    // Element 1 of ResourceContainers gets an object: only that element goes out.
    RepState st = *d.state;
    ArrayValue a = *st.get(s, "ResourceContainers")->get_if<ArrayValue>();
    a.elements[1].values[1] = Value(guid(10341599));
    st.set(s, "ResourceContainers", Value(a));
    BunchWriter delta;
    const size_t bits = write_rep_block(s, st, shadow, delta);
    // bDoChecksum + SIP(66) + count(16) + SIP(2) + GUID(128) + SIP(0) + SIP(0)
    CHECK(bits == 1 + 8 + 16 + 8 + 128 + 8 + 8, "one changed element: its handle and value, after the array count");
    size_t pos = 0;
    const auto back = read_rep_block(s, delta.data(), delta.bit_pos(), pos);
    const ArrayValue* got = back && back->get(s, "ResourceContainers") ? back->get(s, "ResourceContainers")->get_if<ArrayValue>() : nullptr;
    CHECK(back && back->values().size() == 1 && got && got->elements.size() == 3 && got->elements[0].values.empty() &&
              got->elements[1].values.size() == 1 && got->elements[2].values.empty(),
          "it reads back as count 3 with element 1 alone (the other properties and elements are not repeated)");

    // Growing the array sends the count and the new element only.
    ArrayValue grown = a;
    grown.elements.resize(4);
    grown.elements[3].values[1] = Value(guid(10341600));
    st.set(s, "ResourceContainers", Value(grown));
    BunchWriter grow;
    const size_t g = write_rep_block(s, st, shadow, grow);
    CHECK(g == 1 + 8 + 16 + 8 + 128 + 8 + 8, "a fourth element: the count and that element's handle 4");
    // Shrinking sends the count.
    grown.elements.resize(1);
    st.set(s, "ResourceContainers", Value(grown));
    BunchWriter shrink;
    CHECK(write_rep_block(s, st, shadow, shrink) == 1 + 8 + 16 + 8 + 8, "shrinking to one element: the count and no element");
    CHECK(write_rep_block(s, st, shadow, shrink) == 0, "and the shadow then holds the one-element array");
// C:Y:B.CoA

    // A nested array in an element compares recursively.
    const Block& pb = capture_blocks::kPlayerInfoPossessed;
    const auto& ps = schema_of(pb);
    Decoded pd = decode(pb);
    RepShadow pshadow;
    BunchWriter pw;
    write_rep_block(ps, *pd.state, pshadow, pw);
    RepState pst = *pd.state;
    ArrayValue outer = *pst.get(ps, "WorldAttunements.AttunedBlocks")->get_if<ArrayValue>();
    ArrayValue inner = *outer.elements[0].values.at(1).get_if<ArrayValue>();
    inner.elements[0].values[1] = Value::u64(3);
    outer.elements[0].values[1] = Value(inner);
    pst.set(ps, "WorldAttunements.AttunedBlocks", Value(outer));
    BunchWriter pd2;
    const size_t nb = write_rep_block(ps, pst, pshadow, pd2);
    // bDoChecksum + SIP(58) + outer count(16) + SIP(1) + inner count(16) + SIP(1) + uint8 value + SIP(0) x3
    CHECK(nb == 1 + 8 + 16 + 8 + 16 + 8 + 8 + 8 + 8 + 8, "a changed value inside a nested array writes the path of handles down to it");
}

static void test_set_refusals() {
    std::printf("RepState::set refuses what the schema does not carry\n");
    const auto& pawn = schema_of(capture_blocks::kPawnPossessed107);
    RepState st;
    CHECK(!st.set(pawn, "NoSuchProperty", Value::u64(0)), "an unknown path");
    CHECK(!st.set(pawn, 0u, Value::u64(0)) && !st.set(pawn, 9999u, Value::u64(0)), "handle 0 and a handle beyond the commands");
    CHECK(!st.set(pawn, "bHidden", Value::u64(1)), "a bool command with an unsigned value");
    CHECK(!st.set(pawn, "RemoteRole", Value::boolean(true)), "an enum command with a bool");
    CHECK(!st.set(pawn, "RemoteRole", Value::u64(8)), "an enum of 3 bits with 8");
    CHECK(st.set(pawn, "RemoteRole", Value::u64(7)), "and with 7 is fine");
    const uint32_t role = pawn.command("RemoteRole")->handle;
    CHECK(!st.get(role + 65536) && !st.erase(role + 65536) && st.get(role),
          "an out-of-range handle cannot alias or erase a valid 16-bit handle");
    CHECK(!st.set(pawn, "Owner", Value::u64(5)), "an object command with an integer");
    CHECK(!st.set(pawn, "Owner", Value(guid(1))), "an object command with ObjectId 1 (the export form)");
    CHECK(!st.set(pawn, "ReplicatedMovement", Value(Vec3d{})), "an FRepMovement command with an FVector");
    CHECK(!st.set(pawn, "ReplicatedBasedMovement.BaseID", Value::u64(65536)), "a 16-bit command with 65536");
    CHECK(!st.set(pawn, "ReplicatedMovementMode", Value::i64(1)), "an unsigned command with a signed value");
    CHECK(!st.set(pawn, "JumpMaxCount", Value::i64(int64_t(1) << 31)), "a signed 32-bit command with 2^31");
    CHECK(st.set(pawn, "JumpMaxCount", Value::i64(-5)), "and with -5 is fine");
    CHECK(st.get(pawn, "RemoteRole") && st.get(pawn, "JumpMaxCount") && !st.get(pawn, "bHidden") && st.values().size() == 2,
          "refused values leave no trace");
    CHECK(!st.set(pawn, "RepRootMotion.AuthoritativeRootMotion", Value(Vec3d{})),
          "a struct without a codec (FRootMotionSourceGroup)");

    const auto& info = schema_of(capture_blocks::kPlayerInfoPossessed);
    CHECK(!st.set(info, "HeldItems.Items", Value(ArrayValue{})), "a custom-delta (fast array) property: it goes in the field loop");
    CHECK(!st.set(info, "CharacterName", Value::str("bad \xff utf8")), "a string that is not UTF-8");
    ArrayValue bad;
    bad.elements.resize(1);
    bad.elements[0].values[1] = Value::str("wrong kind");
    CHECK(!st.set(info, "WorldAttunements.AttunedBlocks", Value(bad)), "an array whose element has the wrong kind");
    ArrayValue bad_handle;
    bad_handle.elements.resize(1);
    bad_handle.elements[0].values[5] = Value::u64(1);
    CHECK(!st.set(info, "WorldAttunements.AttunedBlocks", Value(bad_handle)), "an array element with a local handle beyond the element commands");

    const auto* align = gen::find_class_schema("UAlignmentComponent");
    CHECK(align && !RepState().set(*align, "Alignments", Value(Vec3d{})), "AoC's FAlignmentContainer needs its own value type");
}

static void test_write_failure() {
    std::printf("a value the wire cannot carry\n");
    const auto& s = schema_of(capture_blocks::kPawnOpenActor);
    RepMovement m;
    m.rotation = Rot{0, 300, 0};   // beyond a byte component
    RepState st;
    st.set(s, "AuthServerIDReplicated", Value::u64(0));
    st.set(s, "ReplicatedMovement", Value(m));
    BunchWriter w;
    w.write_bits(5, 3);
    RepShadow shadow;
    bool failed = false;
    CHECK(write_rep_block(s, st, shadow, w, {}, &failed) == 0 && failed && w.bit_pos() == 3 && shadow.empty(),
          "write_rep_block returns 0 with failed set, the writer as it was and the shadow untouched");
    CodecOptions shorts;
    shorts.rep_movement_short_rotation = true;
    failed = true;
    CHECK(write_rep_block(s, st, shadow, w, shorts, &failed) > 0 && !failed, "with short rotation components it is written");
}
// ~ Cyb :: C O A ~

static void test_read_refusals() {
    std::printf("read_rep_block refuses malformed blocks\n");
    const auto& s = schema_of(capture_blocks::kPawnPossessed107);
    auto attempt = [&](const BunchWriter& w, size_t cut = 0) {
        size_t pos = 0;
        const auto r = read_rep_block(s, w.data(), w.bit_pos() - cut, pos);
        return std::make_pair(r.has_value(), pos);
    };
    {
        BunchWriter w;
        w.write_bit(1);          // bDoChecksum
        w.write_sip(0);
        CHECK(!attempt(w).first, "a set bDoChecksum bit");
    }
    {
        BunchWriter w;
        w.write_bit(0);
        w.write_sip(3); w.write_bit(1);
        w.write_sip(2); w.write_bit(1);   // not ascending
        w.write_sip(0);
        CHECK(!attempt(w).first, "handles that do not ascend");
    }
    {
        BunchWriter w;
        w.write_bit(0);
        w.write_sip(4000); w.write_bit(1);
        w.write_sip(0);
        CHECK(!attempt(w).first, "a handle beyond the class's commands");
    }
    {
        const auto& stats = *gen::find_class_schema("UAoCStatsComponent");
        BunchWriter w;
        w.write_bit(0);
        w.write_sip(4);                 // StatRepInt32Owner.Items: custom delta
        w.write_bits(0, 16); w.write_sip(0); w.write_sip(0);
        size_t pos = 0;
        CHECK(!read_rep_block(stats, w.data(), w.bit_pos(), pos).has_value(), "a custom-delta handle (fast array)");
    }
    {
        BunchWriter w;
        w.write_bit(0);
        w.write_sip(3); w.write_bit(1);   // no closing handle 0
        CHECK(!attempt(w).first, "a block without its closing handle 0");
    }
    {
        const auto bytes = capture_blocks::bytes_of(capture_blocks::kPawnPossessed107.hex);
        size_t pos = 0;
        CHECK(!read_rep_block(s, bytes.data(), 470, pos).has_value() && pos == 0, "a block cut short (pos stays)");
        pos = 0;
        CHECK(read_rep_block(s, bytes.data(), 477, pos).has_value() && pos == 477, "the whole block reads and pos moves to its end");
        pos = 477;
        CHECK(!read_rep_block(s, bytes.data(), 477, pos).has_value(), "no block at the end of the data");
    }
    {
        // A block that starts mid-buffer (as content blocks do): pkt#2523's 18 bits after 5 padding bits.
        const auto bytes = capture_blocks::bytes_of(capture_blocks::kPawnUnhide2523.hex);
        BunchWriter w;
        w.write_bits(0x15, 5);
        w.write_bit_range(bytes.data(), 0, 18);
        size_t pos = 5;
        const auto r = read_rep_block(s, w.data(), w.bit_pos(), pos);
        CHECK(r && pos == 23 && r->get(s, "bHidden") && *r->get(s, "bHidden") == Value::boolean(false), "a block at a bit offset reads and leaves pos after it");
    }
}

static void test_field_loop() {
    std::printf("field loop header after the block: UPlayerInfo HeldItems\n");
    const Block& b = capture_blocks::kPlayerInfoPossessed;
    const auto& s = schema_of(b);
    const auto bytes = capture_blocks::bytes_of(b.hex);
    Decoded d = decode(b);
    CHECK(d.state && d.pos == 165 && b.bits == 2868, "the RepLayout block is 165 of the 2868 bits");
    size_t pos = d.pos;
    const auto h = read_field_header(s, bytes.data(), b.bits, pos);
    CHECK(h && h->field == 28 && h->payload_bits == 2682 && pos + h->payload_bits == b.bits,
          "field 28 with a 2682-bit payload, ending the block");
    CHECK(s.field(28) && std::string(s.field(28)->name) == "HeldItems", "field 28 is HeldItems");

    BunchWriter w;
    CHECK(write_field_header(s, 28, 2682, w), "write_field_header accepts field 28");
    const auto captured_header = slice_bits(bytes, d.pos, w.bit_pos());
    CHECK(pos - d.pos == w.bit_pos() && capture_blocks::same_bit_range(w.data(), captured_header.data(), w.bit_pos(), nullptr),
          "and the header is the captured bits (SerializeInt(28, 198) + SIP(2682))");

    BunchWriter payload;
    payload.write_bits(0x2d, 6);
    BunchWriter header6;
    write_field_header(s, 28, 6, header6);
    BunchWriter framed;
    CHECK(write_field(s, 28, payload, framed) == header6.bit_pos() + 6, "write_field appends the payload's bits after the header");
    size_t p2 = 0;
    const auto back = read_field_header(s, framed.data(), framed.bit_pos(), p2);
    CHECK(back && back->field == 28 && back->payload_bits == 6 && p2 == header6.bit_pos(), "and read_field_header reads it back");

    BunchWriter bad;
    CHECK(!write_field_header(s, s.num_fields(), 0, bad) && bad.bit_pos() == 0, "a field beyond the class is not written");
    BunchWriter over;
    write_field_header(s, 28, 100, over);
    size_t p3 = 0;
    CHECK(!read_field_header(s, over.data(), over.bit_pos(), p3).has_value() && p3 == 0, "a payload that runs past the block is refused");
    BunchWriter beyond;
    beyond.write_serialize_int(s.num_fields(), s.field_max());   // field_max - 1: the value NumFields names no field
    beyond.write_sip(0);
    size_t p4 = 0;
    CHECK(!read_field_header(s, beyond.data(), beyond.bit_pos(), p4).has_value(), "field number NumFields is refused");
}

// ---- hostile counts, handles and values ----------------------------------------------------------------------------------

static const schema::ClassSchema& schema_named(const char* name) {
    const schema::ClassSchema* s = gen::find_class_schema(name);
    if (!s) { std::printf("  [FAIL] no schema %s\n", name); std::exit(2); }
    return *s;
}

// Live and peak allocation, for the amplification test: every allocation of this executable goes through here.
static size_t g_live = 0, g_peak = 0;
// (C O A + C_Y_B)

void* operator new(size_t n) {
    void* p = std::malloc(n + 16);
    if (!p) throw std::bad_alloc();
    *static_cast<size_t*>(p) = n;
    g_live += n;
    if (g_live > g_peak) g_peak = g_live;
    return static_cast<char*>(p) + 16;
}
void operator delete(void* q) noexcept {
    if (!q) return;
    char* p = static_cast<char*>(q) - 16;
    g_live -= *reinterpret_cast<size_t*>(p);
    std::free(p);
}
void operator delete(void* q, size_t) noexcept { operator delete(q); }

static void test_hostile_counts() {
    std::printf("a hostile array count builds a bounded number of elements\n");
    const schema::ClassSchema& info = schema_named("UPlayerInfo");
    const schema::RepCommand* attuned = info.command("WorldAttunements.AttunedBlocks");
    CHECK(attuned && attuned->type == schema::PropType::DynamicArray && info.elements(*attuned).size() == 1 &&
              info.elements(*attuned)[0].type == schema::PropType::DynamicArray,
          "WorldAttunements.AttunedBlocks: an array whose element is an array");
    if (!attuned) return;
    // review B's probe: an outer array of `outer` elements, then `nested` of them each with an inner array of `inner` elements
    auto block = [&](uint64_t outer, uint64_t inner, size_t nested) {
        BunchWriter w;
        w.write_bit(0);
        w.write_sip(attuned->handle);
        w.write_bits(outer, 16);
        for (size_t k = 0; k < nested; ++k) {
            w.write_sip(k + 1);     // element k, local handle 1 (AttunedIndices)
            w.write_bits(inner, 16);
            w.write_sip(0);         // closes the inner array
        }
        w.write_sip(0);             // closes the outer array
        w.write_sip(0);             // closes the block
        return w;
    };
    struct Result { bool ok; size_t peak; };
    auto attempt = [&](const schema::ClassSchema& cls, const BunchWriter& w) {
        size_t pos = 0;
        const size_t base = g_live;
        g_peak = base;
        const bool ok = read_rep_block(cls, w.data(), w.bit_pos(), pos).has_value();
        return Result{ok, g_peak - base};
    };
    {
        const BunchWriter w = block(65535, 65535, 20);   // 86 bytes: without the budget it asks for ~370 MB
        const Result r = attempt(info, w);
        std::printf("    %zu bytes of input: peak allocation %zu bytes\n", w.byte_size(), r.peak);
        CHECK(!r.ok && r.peak < (1u << 20), "86 bytes of 65535-element arrays within 65535-element arrays: refused without the allocation");
    }
    {
        const BunchWriter w = block(65535, 65535, 4000);   // 16 KB: without the budget it asks for tens of GB and a minute
        const Result r = attempt(info, w);
        CHECK(!r.ok && r.peak < (1u << 20), "16 KB of the same: refused, quickly");
    }
    {
        const BunchWriter w = block(kMaxBlockElements, kMaxBlockElements, 500);   // the outer array fits the budget, no inner one does
        const Result r = attempt(info, w);
        std::printf("    %zu bytes of input: peak allocation %zu bytes\n", w.byte_size(), r.peak);
        CHECK(!r.ok && r.peak < (8u << 20), "an outer array that uses the whole budget leaves none for the inner ones: refused, at most one array built");
    }
    // The budget is one block's, all its arrays together.
    const schema::ClassSchema& owner = schema_named("UOwnerInfoComponent");
    const schema::RepCommand* containers = owner.command("ResourceContainers");
    const schema::RepCommand* attackers = owner.command("NpcAttackers");
    CHECK(containers && attackers && containers->type == schema::PropType::DynamicArray && attackers->type == schema::PropType::DynamicArray,
          "UOwnerInfoComponent has the arrays ResourceContainers and NpcAttackers");
    if (!containers || !attackers) return;
    auto two_arrays = [&](uint64_t first, uint64_t second) {
        BunchWriter w;
        w.write_bit(0);
        const schema::RepCommand* lo = containers->handle < attackers->handle ? containers : attackers;
        const schema::RepCommand* hi = lo == containers ? attackers : containers;
        w.write_sip(lo->handle);
        w.write_bits(lo == containers ? first : second, 16);
        w.write_sip(0);
        w.write_sip(hi->handle);
        w.write_bits(hi == containers ? first : second, 16);
        w.write_sip(0);
        w.write_sip(0);
        return w;
    };
    CHECK(attempt(owner, two_arrays(kMaxBlockElements / 2, kMaxBlockElements / 2)).ok, "two arrays that together fit the budget read");
    CHECK(!attempt(owner, two_arrays(kMaxBlockElements / 2, kMaxBlockElements / 2 + 1)).ok, "one element more in all: refused");
    CHECK(attempt(owner, two_arrays(kMaxBlockElements, 0)).ok && !attempt(owner, two_arrays(kMaxBlockElements + 1, 0)).ok,
          "an array of exactly kMaxBlockElements reads, one more does not");
    {   // read_value on its own, outside any block, has a budget of its own
        ArrayValue three;
        three.elements.resize(3);
        three.elements[1].values[1] = Value(guid(10341599));
        BunchWriter w;
        const bool wrote = write_value(*containers, owner.element_commands, Value(three), w);
        wire::PacketReader r(w.data(), w.byte_size(), w.bit_pos());
        const auto v = wrote ? read_value(*containers, owner.element_commands, r) : std::nullopt;
        CHECK(v && *v == Value(three) && r.pos() == w.bit_pos(), "read_value reads an array on its own");
    }
}

// ---- T5: array element handles in a RepLayout block ------------------------------------------------------------------------------
static void t5_array_element_handles() {
    std::printf("array element handles that do not fit the count or do not ascend\n");
    const schema::ClassSchema& owner = schema_named("UOwnerInfoComponent");
    const schema::RepCommand* arr = owner.command("ResourceContainers");
    auto block = [&](uint64_t count, std::initializer_list<uint64_t> handles) {
        BunchWriter w;
        w.write_bit(0);
        w.write_sip(arr->handle);
        w.write_bits(count, 16);
        for (uint64_t h : handles) {
            w.write_sip(h);
            emit::write_intrepid_guid(w, guid(10341599));
        }
        w.write_sip(0);   // closes the array
        w.write_sip(0);   // closes the block
        return w;
    };
    auto reads = [&](const BunchWriter& w) {
        size_t pos = 0;
        return read_rep_block(owner, w.data(), w.bit_pos(), pos).has_value();
    };
    CHECK(arr && arr->type == schema::PropType::DynamicArray && arr->elem_end - arr->elem_begin == 1, "ResourceContainers: an array of one command per element");
    CHECK(reads(block(3, {1, 3})), "control: elements 0 and 2 of 3");
    CHECK(!reads(block(2, {3})), "element 2 of 2 (handle 3): refused");
    CHECK(!reads(block(0, {1})), "element 0 of an empty array: refused");
    CHECK(!reads(block(3, {2, 1})), "element handles 2 then 1 (descending): refused");
    CHECK(!reads(block(3, {2, 2})), "element handle 2 twice: refused");
}

// ---- T7: RepState::set bounds an array at 65535 elements ---------------------------------------------------------------------
static void t7_array_bound() {
    std::printf("an array of 65536 elements\n");
    const schema::ClassSchema& owner = schema_named("UOwnerInfoComponent");
    ArrayValue big;
    big.elements.resize(65536);
    RepState st;
    CHECK(!st.set(owner, "ResourceContainers", Value(big)), "65536 elements do not fit the 16-bit count");
    big.elements.resize(65535);
    CHECK(st.set(owner, "ResourceContainers", Value(big)), "65535 do");
}

// ---- RepState::set accepts what the writer writes ---------------------------------------------------------------------------------
static void test_set_matches_writer() {
    std::printf("RepState::set accepts exactly what the writer can write\n");
    const auto& loc = schema_named("PlayerPawn_C");   // AttachmentReplication.LocationOffset: FVector_NetQuantize100
    const auto& info = schema_named("UPlayerInfo");   // CharacterName: FString
    const auto& ps = schema_named("AoCPlayerStateBP_C");
    const schema::RepCommand* unique_id = ps.command("UniqueId");
    // The value is accepted by set and then written and read back; or refused by set.
    auto trips = [&](const schema::ClassSchema& cls, const char* path, const Value& v) {
        RepState st;
        if (!st.set(cls, path, v)) return false;
        BunchWriter w;
        RepShadow sh;
        bool failed = true;
        const size_t n = write_rep_block(cls, st, sh, w, {}, &failed);
        size_t pos = 0;
        const auto back = failed || !n ? std::nullopt : read_rep_block(cls, w.data(), w.bit_pos(), pos);
        return back && *back == st;
    };
    auto refused = [&](const schema::ClassSchema& cls, const char* path, const Value& v) { return !RepState().set(cls, path, v); };
    const char* offset = "AttachmentReplication.LocationOffset";
// (C:Y:B | CoA)

    // quantized vectors: at most 63 bits a component (UE::Net::WriteQuantizedVector)
    const int64_t big63 = (int64_t(1) << 62) - 1;
    CHECK(trips(loc, offset, Value(QuantVec{QuantVec::Mode::Scaled, big63, -(int64_t(1) << 62), 5})), "a quantized component of 63 bits is written and read back");
    CHECK(refused(loc, offset, Value(QuantVec{QuantVec::Mode::Scaled, INT64_MAX, 0, 0})), "INT64_MAX (64 bits) is refused");
    CHECK(refused(loc, offset, Value(QuantVec{QuantVec::Mode::Scaled, INT64_MIN, 0, 0})), "INT64_MIN is refused");
    CHECK(refused(loc, offset, Value(QuantVec{QuantVec::Mode::Unscaled, int64_t(1) << 62, 0, 0})), "2^62 (64 bits) is refused");
    CHECK(refused(loc, offset, Value(QuantVec{QuantVec::Mode::Scaled, 0, -(int64_t(1) << 62) - 1, 0})), "-2^62 - 1 is refused");
    BunchWriter scratch;
    CHECK(!write_quantized_vector(scratch, QuantVec{QuantVec::Mode::Scaled, INT64_MAX, 0, 0}) && scratch.bit_pos() == 0 &&
              !quant_vec_fits(QuantVec{QuantVec::Mode::Scaled, INT64_MAX, 0, 0}) && quant_vec_fits(QuantVec{QuantVec::Mode::Scaled, big63, 0, 0}),
          "the writer refuses the same values and writes nothing");
    CHECK(trips(loc, offset, Value(QuantVec{QuantVec::Mode::RawDouble, INT64_MIN, 0, INT64_MAX})), "full precision doubles carry any bits");
    CHECK(trips(loc, offset, Value(QuantVec{QuantVec::Mode::RawFloat, 0x3f800000, 0, UINT32_MAX})), "a full precision float vector carries 32 bits");
    CHECK(refused(loc, offset, Value(QuantVec{QuantVec::Mode::RawFloat, int64_t(UINT32_MAX) + 1, 0, 0})), "a float component beyond 32 bits is refused");
    CHECK(refused(loc, offset, Value(QuantVec{QuantVec::Mode::RawFloat, -1, 0, 0})), "and a negative one");

    // FRepMovement: the embedded vectors
    RepMovement m;
    m.location = QuantVec{QuantVec::Mode::Scaled, 100, 200, 300};
    CHECK(trips(loc, "ReplicatedMovement", Value(m)), "an FRepMovement with small vectors is written and read back");
    RepMovement wide = m;
    wide.location.x = INT64_MAX;
    CHECK(refused(loc, "ReplicatedMovement", Value(wide)), "a 64-bit location component is refused");
    wide = m;
    wide.linear_velocity.z = INT64_MIN;
    CHECK(refused(loc, "ReplicatedMovement", Value(wide)), "a 64-bit velocity component is refused");
    wide = m;
    wide.rep_physics = true;
    wide.angular_velocity.y = INT64_MAX;
    CHECK(refused(loc, "ReplicatedMovement", Value(wide)), "a 64-bit angular velocity is refused when it goes out (bRepPhysics)");
    wide = m;
    wide.acceleration = QuantVec{QuantVec::Mode::Scaled, 0, 0, INT64_MAX};
    CHECK(refused(loc, "ReplicatedMovement", Value(wide)), "a 64-bit acceleration is refused");
    wide = m;
    wide.rotation = Rot{0, 300, 0};
    RepState bytes_rotation;
    BunchWriter rw;
    RepShadow rs;
    bool failed = false;
    CHECK(bytes_rotation.set(loc, "ReplicatedMovement", Value(wide)) && write_rep_block(loc, bytes_rotation, rs, rw, {}, &failed) == 0 && failed,
          "a rotation beyond a byte stays a write-time failure: set cannot know the object's rotation quantization");

    // FUniqueNetIdRepl: a non-V2 encoded id has a byte for its length
    CHECK(unique_id != nullptr, "AoCPlayerStateBP_C.UniqueId");
    if (unique_id) {
        UniqueNetId id;
        id.flags = 1 | (5 << 3);
        id.bytes.assign(255, 7);
        CHECK(trips(ps, "UniqueId", Value(id)), "an encoded id of 255 bytes is written and read back");
        id.bytes.assign(256, 7);
        CHECK(refused(ps, "UniqueId", Value(id)), "256 bytes are refused (the length is a byte)");
        id.flags = 1 | (30 << 3);   // TypeHash_V2: an int32 length
        id.services = 3;
        id.bytes.assign(300, 7);
        CHECK(trips(ps, "UniqueId", Value(id)), "a V2 id of 300 bytes is fine");
        UniqueNetId named;
        named.flags = 1 | (31 << 3);
        named.type = std::string("a\0b", 3);
        CHECK(refused(ps, "UniqueId", Value(named)), "a type name with a NUL is refused");
        UniqueNetId text;
        text.flags = 8;
        text.id = std::string("x\0y", 3);
        CHECK(refused(ps, "UniqueId", Value(text)), "an unencoded id string with a NUL is refused");
        text.id = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
        CHECK(trips(ps, "UniqueId", Value(text)), "and a plain one is fine");
        // what the flags say is not written is not checked
        UniqueNetId empty;
        empty.flags = 3;   // encoded and empty
        empty.bytes.assign(1000, 1);
        CHECK(struct_accepts("FUniqueNetIdRepl", Value(empty)), "an empty id has no bytes on the wire: its leftover bytes do not matter");
    }

    // strings: no NUL, at most 65,535 characters
    CHECK(trips(info, "CharacterName", Value::str(std::string(65535, 'x'))), "a string of 65,535 characters is written and read back");
    CHECK(refused(info, "CharacterName", Value::str(std::string(65536, 'x'))), "65,536 characters are refused");
    CHECK(refused(info, "CharacterName", Value::str(std::string("ab\0cd", 5))), "a string with an embedded NUL is refused");
    std::string accents;
    for (int i = 0; i < 65535; ++i) accents += "\xC3\xA9";   // U+00E9: two bytes, one UCS-2 unit
    CHECK(trips(info, "CharacterName", Value::str(accents)), "65,535 two-byte characters (131,070 bytes: the limit counts characters) are fine");
    accents += "\xC3\xA9";
    CHECK(refused(info, "CharacterName", Value::str(accents)), "and 65,536 are not");
    std::string faces;
    for (int i = 0; i < 32768; ++i) faces += "\xF0\x9F\x98\x80";   // U+1F600: a surrogate pair, two units
    CHECK(refused(info, "CharacterName", Value::str(faces)), "32,768 surrogate pairs (65,536 UCS-2 units) are refused");
    CHECK(refused(info, "CharacterName", Value::str(std::string("\xC3\xA9\0", 3))), "a NUL after a non-ASCII character is refused too");
    BunchWriter sw;
    CHECK(!write_fstring(sw, std::string(65536, 'x')) && !write_fstring(sw, std::string("a\0b", 3)) && sw.bit_pos() == 0 &&
              write_fstring(sw, std::string(65535, 'x')),
          "write_fstring refuses the same and writes nothing");

    // names, texts, soft objects and tags by their commands
    schema::RepCommand cmd{};
    cmd.type = schema::PropType::Name;
    const std::string nul("n\0m", 3);
    CHECK(!value_matches(cmd, {}, Value(NameValue::plain(nul))) && value_matches(cmd, {}, Value(NameValue::plain("ok"))) &&
              !value_matches(cmd, {}, Value(NameValue::plain(std::string(65536, 'n')))),
          "an FName with a NUL or a string of 65,536 characters is refused");
    NameValue hard = NameValue::hardcoded_name(5);
    hard.text = nul;
    CHECK(value_matches(cmd, {}, Value(hard)), "a hardcoded name has no text on the wire: its leftover text does not matter");
    cmd.type = schema::PropType::Text;
    TextValue t;
    t.history = 0;
    t.ns = "UI";
    t.key = "Key";
    t.source = "text";
    CHECK(value_matches(cmd, {}, Value(t)), "an FText of the Base history");
    t.key = nul;
    CHECK(!value_matches(cmd, {}, Value(t)), "with a NUL in its key is refused");
    t.key = "Key";
    t.history = 3;
    CHECK(!value_matches(cmd, {}, Value(t)), "a history that is not decoded is refused");
    TextValue none;
    none.history = 0xFF;
    none.has_string = true;
    none.source = std::string(65536, 's');
    CHECK(!value_matches(cmd, {}, Value(none)), "an FText string of 65,536 characters is refused");
    none.has_string = false;
    CHECK(value_matches(cmd, {}, Value(none)), "and is not checked when it does not go out");
    cmd.type = schema::PropType::SoftObject;
    SoftObjectValue so;
    so.is_path = true;
    so.path = nul;
    CHECK(!value_matches(cmd, {}, Value(so)), "a soft object path with a NUL is refused");
    so.path = "/Game/A";
    CHECK(value_matches(cmd, {}, Value(so)), "and a plain one is fine");
    CHECK(!struct_accepts("FGameplayTag", Value(GameplayTag{NameValue::plain(nul)})) &&
              !struct_accepts("FGameplayTagContainer", Value(GameplayTagContainer{{NameValue::plain("ok"), NameValue::plain(nul)}})) &&
              struct_accepts("FGameplayTagContainer", Value(GameplayTagContainer{{NameValue::plain("ok")}})),
          "a gameplay tag or a tag of a container with a NUL is refused");
}
// [C.O.A - C.Y.B]

// ---- an element value that is gone ---------------------------------------------------------------------------------------------------
static void test_removed_element_values() {
    std::printf("an array element value that was sent and is gone is sent as its zero\n");
    const schema::ClassSchema& owner = schema_named("UOwnerInfoComponent");
    ArrayValue a;
    a.elements.resize(3);
    a.elements[1].values[1] = Value(guid(10341599));
    RepState st;
    RepShadow shadow;
    BunchWriter w;
    st.set(owner, "ResourceContainers", Value(a));
    // bDoChecksum + SIP(66) + count(16) + SIP(2) + GUID(128) + SIP(0) + SIP(0)
    CHECK(write_rep_block(owner, st, shadow, w) == 1 + 8 + 16 + 8 + 128 + 8 + 8, "the first send: element 1 alone");
    a.elements[1].values.clear();   // the server clears it: the client still holds the GUID
    st.set(owner, "ResourceContainers", Value(a));
    BunchWriter cleared;
    bool failed = true;
    const size_t bits = write_rep_block(owner, st, shadow, cleared, {}, &failed);
    CHECK(bits == 1 + 8 + 16 + 8 + 128 + 8 + 8 && !failed, "clearing it writes the element again, as the null object");
    size_t pos = 0;
    const auto back = read_rep_block(owner, cleared.data(), cleared.bit_pos(), pos);
    const Value* arr = back ? back->get(owner, "ResourceContainers") : nullptr;
    const ArrayValue* got = arr ? arr->get_if<ArrayValue>() : nullptr;
    CHECK(got && got->elements.size() == 3 && got->elements[1].values.size() == 1 &&
              got->elements[1].values.at(1) == Value(emit::FIntrepidNetworkGUID{}),
          "it reads back as element 1 = the null object (the client no longer keeps the stale GUID)");
    CHECK(write_rep_block(owner, st, shadow, cleared) == 0, "and then nothing more: the client has the zero");

    // A zero that was sent is not sent again when the value goes.
    ArrayValue z;
    z.elements.resize(2);
    z.elements[0].values[1] = Value(emit::FIntrepidNetworkGUID{});
    RepState zs;
    RepShadow zsh;
    BunchWriter zw;
    zs.set(owner, "ResourceContainers", Value(z));
    write_rep_block(owner, zs, zsh, zw);
    z.elements[0].values.clear();
    zs.set(owner, "ResourceContainers", Value(z));
    CHECK(write_rep_block(owner, zs, zsh, zw) == 0, "a value that was the zero and is gone sends nothing");

    // A nested array that is gone goes out empty.
    const schema::ClassSchema& info = schema_named("UPlayerInfo");
    ArrayValue inner;   // WorldAttunements.AttunedBlocks = [{AttunedIndices = [1]}]
    inner.elements.resize(1);
    inner.elements[0].values[1] = Value::u64(1);
    ArrayValue outer;
    outer.elements.resize(1);
    outer.elements[0].values[1] = Value(inner);
    RepState is;
    RepShadow ish;
    BunchWriter iw;
    is.set(info, "WorldAttunements.AttunedBlocks", Value(outer));
    write_rep_block(info, is, ish, iw);
    outer.elements[0].values.clear();
    is.set(info, "WorldAttunements.AttunedBlocks", Value(outer));
    BunchWriter ow;
    // bDoChecksum + SIP(58) + outer count(16) + SIP(1) + inner count 0 (16) + SIP(0) inner + SIP(0) outer + SIP(0)
    CHECK(write_rep_block(info, is, ish, ow) == 1 + 8 + 16 + 8 + 16 + 8 + 8 + 8, "a removed inner array goes out as count 0");

    // A type without a known zero cannot be reset: the write fails and nothing changes.
    const schema::ClassSchema& look = schema_named("UCharacterAppearanceComponent");
    ArrayValue ids;
    ids.elements.resize(1);
    ids.elements[0].values[1] = Value(AppearanceId{1, 2, 3, 4});
    RepState ls;
    RepShadow lsh;
    BunchWriter lw;
    ls.set(look, "AppearanceIDs", Value(ids));
    CHECK(write_rep_block(look, ls, lsh, lw) > 0, "AppearanceIDs: one element sent");
    ids.elements[0].values.clear();
    ls.set(look, "AppearanceIDs", Value(ids));
    const RepShadow before = lsh;
    BunchWriter lw2;
    failed = false;
    CHECK(write_rep_block(look, ls, lsh, lw2, {}, &failed) == 0 && failed && lw2.bit_pos() == 0 && lsh.sent == before.sent,
          "an FAppearanceId has no known zero: removing it fails the write, the shadow untouched");
    CHECK(zero_value(*look.command("AppearanceIDs")).has_value() && !zero_value(look.elements(*look.command("AppearanceIDs"))[0]).has_value(),
          "(the array's zero is empty, the struct's is unknown)");
}

// ---- per-connection conditions, at the RepLayout level ---------------------------------------------------------------------------------
static void test_parent_mask() {
    std::printf("the parent mask: an inactive parent is left out of the block and the shadow\n");
    const auto& s = schema_named("PlayerPawn_C");
    RepState st;
    st.set(s, "bHidden", Value::boolean(true));
    RepMovement m;
    m.location = QuantVec{QuantVec::Mode::Scaled, 1, 2, 3};
    st.set(s, "ReplicatedMovement", Value(m));
    const schema::RepCommand* movement = s.command("ReplicatedMovement");
    const schema::RepCommand* hidden = s.command("bHidden");
// ~ C | O | A & C.Y.B ~

    ParentMask mask;
    CHECK(mask.all_active() && mask.active(movement->parent), "everything is active until the caller says otherwise");
    CHECK(mask.set_active(s, "ReplicatedMovement", false) && !mask.all_active() && !mask.active(movement->parent) && mask.active(hidden->parent),
          "set_active by path turns one parent off");
    RepShadow shadow;
    BunchWriter w;
    RepShadowUndo undo;
    bool failed = true;
    const size_t bits = write_rep_block(s, st, shadow, w, {}, &failed, &mask, &undo);
    size_t pos = 0;
    const auto back = bits ? read_rep_block(s, w.data(), w.bit_pos(), pos) : std::nullopt;
    CHECK(!failed && back && back->values().size() == 1 && back->get(hidden->handle) && shadow.sent.size() == 1 && shadow.sent.count(hidden->handle),
          "only the active parent's handle is written and recorded");
    CHECK(undo.entries.size() == 1 && undo.entries[0].first == hidden->handle && !undo.entries[0].second,
          "the undo record has that handle, which the shadow did not hold");
    undo.apply(shadow);
    CHECK(shadow.empty(), "applying it puts the shadow back");

    // everything inactive: nothing to write
    mask.set_active(s, "bHidden", false);
    BunchWriter none;
    CHECK(write_rep_block(s, st, shadow, none, {}, &failed, &mask) == 0 && !failed && none.bit_pos() == 0 && shadow.empty(), "every parent inactive: nothing");
    mask.reset();
    BunchWriter all;
    CHECK(write_rep_block(s, st, shadow, all, {}, &failed, &mask) > 0 && shadow.sent.size() == 2, "reset: everything is active again");

    // the undo of an overwrite gives the old value back
    RepState st2 = st;
    st2.set(s, "bHidden", Value::boolean(false));
    RepShadowUndo undo2;
    BunchWriter again;
    CHECK(write_rep_block(s, st2, shadow, again, {}, &failed, nullptr, &undo2) > 0 && *shadow.sent.at(hidden->handle).get_if<bool>() == false,
          "an update that changes bHidden");
    undo2.apply(shadow);
    CHECK(shadow.sent.size() == 2 && *shadow.sent.at(hidden->handle).get_if<bool>() == true, "its undo restores the old value");

    // An empty array on a first send goes out as count 0 (a few bits the client does not need; UE leaves it out).
    const auto& owner = schema_named("UOwnerInfoComponent");
    RepState es;
    es.set(owner, "ResourceContainers", Value(ArrayValue{}));
    RepShadow esh;
    BunchWriter ew;
    CHECK(write_rep_block(owner, es, esh, ew) == 1 + 8 + 16 + 8 + 8, "an empty array on a first send: count 0");
}

int main() {
    test_captured_blocks();
    test_decoded_values();
    test_built_from_scratch();
    test_shadow();
    test_array_diff();
    test_set_refusals();
    test_write_failure();
    test_read_refusals();
    test_field_loop();
    test_hostile_counts();
    t5_array_element_handles();
    t7_array_bound();
    test_set_matches_writer();
    test_removed_element_values();
    test_parent_mask();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
