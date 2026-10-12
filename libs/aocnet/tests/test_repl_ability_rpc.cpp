// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_ability_rpc.cpp
//
//  Plan A7, step 1: the ability RPCs the client sends (protocol/repl/ability_rpc.h) and the
//  FItemInstance codec they needed.
//
//    * FItemInstance: the empty item is a single 0 bit (the whole codec is item_codec.h's, tested by
//      test_repl_items); an instance whose bits run out after the has-item bit is refused.
//    * All 54 captured calls (tests/capture_abilities.h: the e2e capture's 16 ServerTriggerAbility
//      and 4 ServerNotifyHeldAbilityCancelled, the 2026-09-29 native run's 31 and 3) are read by
//      read_client_bunch to their last bit with `decoded` true, and write back to the client's own
//      bits.  The parse helpers give what the research found in all 308 calls of 15 captures:
//      ClientInstanceId counts every trigger from 0 without a gap, every cancel names an earlier
//      trigger, ItemData is "no item" (Slot -1, Location 24), bPvpForceFlag is 0.
//    * ClientEndAbility(ClientInstanceId), the server's call, is built and read back by the
//      schema-driven reader.
//    * The parse helpers refuse another RPC and a call whose parameters were not decoded.
// ============================================================================
#include "capture_abilities.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/rep_block.h"
#include "protocol/repl/subobject.h"
#include "protocol/schema/generated/index.h"
#include "protocol/wire/packet_reader.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::BunchWriter;
using emit::FIntrepidNetworkGUID;
using wire::PacketReader;
namespace gen = aoc::protocol::schema::gen;
namespace cap = capture_abilities;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
// C.O.A and Cyb

// The abilities of the captures (research 2026-10-01): Sprint, Dodge, the Scepter combo, QuickWit, Guard.
constexpr int64_t kSprint = 6064631641648332884ll;
constexpr int64_t kDodge = 107258680465016ll;
constexpr int64_t kScepterCombo = 6064631845843960261ll;
constexpr int64_t kQuickWit = 6064630736949673984ll;
constexpr int64_t kGuard = 107887054050936ll;

static const schema::ClassSchema& schema_named(const char* name) {
    const schema::ClassSchema* s = gen::find_class_schema(name);
    if (!s) { std::printf("  [FAIL] no schema %s\n", name); std::exit(2); }
    return *s;
}

static std::vector<uint8_t> bytes_of_hex(const char* hex) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    std::vector<uint8_t> out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) out.push_back(static_cast<uint8_t>(nib(hex[i]) << 4 | nib(hex[i + 1])));
    return out;
}

/// The bits [from, from + n) of `bytes`, as a buffer starting at bit 0.
static std::vector<uint8_t> slice_bits(const std::vector<uint8_t>& bytes, size_t from, size_t n) {
    std::vector<uint8_t> out((n + 7) / 8, 0);
    for (size_t i = 0; i < n; ++i) out[i >> 3] |= ((bytes[(from + i) >> 3] >> ((from + i) & 7)) & 1) << (i & 7);
    return out;
}

static void test_item_instance_codec() {
    std::printf("FItemInstance: the empty item (the full codec: test_repl_items)\n");
    CHECK(is_known_struct("FItemInstance"), "FItemInstance has a codec");
    BunchWriter w;
    CHECK(write_struct("FItemInstance", Value(ItemInstance{}), w) && w.bit_pos() == 1 && (w.data()[0] & 1) == 0,
          "the empty item is written as a single 0 bit");
    PacketReader r(w.data(), w.byte_size(), w.bit_pos());
    const auto back = read_struct("FItemInstance", r);
    CHECK(back && *back == Value(ItemInstance{}) && r.pos() == 1, "and reads back as the empty item");

    BunchWriter set;
    set.write_bits(0xffff, 16);
    PacketReader rs(set.data(), set.byte_size(), set.bit_pos());
    CHECK(!read_struct("FItemInstance", rs).has_value(), "a set bit with no instance behind it is a read past the end: refused");
    BunchWriter none;
    PacketReader rn(none.data(), none.byte_size(), 0);
    CHECK(!read_struct("FItemInstance", rn).has_value(), "no bit at all is a read past the end");
    BunchWriter bad;
    CHECK(!write_struct("FItemInstance", Value(Vec3d{}), bad) && bad.bit_pos() == 0, "the writer refuses a value that is not an instance");
    CHECK(struct_accepts("FItemInstance", Value(ItemInstance{})) && !struct_accepts("FItemInstance", Value::u64(0)),
          "struct_accepts takes an instance (the default one is the empty item), nothing else");
    CHECK(Value(ItemInstance{}).kind() == ValueKind::Struct && !zero_struct("FItemInstance").has_value(),
          "it is a struct value, and its zero form is not claimed");
}

/// What the test knows of one captured call.
struct Call {
    TriggerAbility   trigger;
    HeldAbilityCancelled cancel;
    bool             is_trigger = false;
};

static const schema::ClassSchema& pawn() { return schema_named("PlayerPawn_C"); }
static const schema::ClassSchema& ability() { return schema_named("UAoCAbilityComponent"); }

static const schema::ClassSchema* resolve_ability(const FIntrepidNetworkGUID& g) {
    return g.ObjectId == cap::kAbilityObjectId ? &ability() : nullptr;
}

/// Reads every bunch of a capture; the calls in order.  Checks each decodes and writes back to the client's bits.
/// The client names a Scepter combo's target by path: ObjectId 1 and the export (value.h InlineObject).
static CodecOptions client_options() {
    CodecOptions o;
    o.inline_objects = true;
    return o;
}
// C:Y:B x CoA

static std::vector<Call> read_capture(const char* label, const cap::Bunch* bunches, size_t n, size_t want_triggers,
                                      size_t want_cancels, size_t* inline_targets = nullptr) {
    std::vector<Call> calls;
    size_t triggers = 0, cancels = 0, undecoded = 0, rewritten = 0, other = 0, expected = 0;
    bool all_read = true;
    for (size_t i = 0; i < n; ++i) {
        const cap::Bunch& b = bunches[i];
        const auto bytes = bytes_of_hex(b.hex);
        const auto parsed = read_client_bunch(pawn(), resolve_ability, bytes.data(), b.bits, client_options());
        if (!parsed) {
            all_read = false;
            continue;
        }
        expected += b.triggers + b.cancels;
        for (const ClientRpc& rpc : parsed->rpcs) {
            const std::string name = rpc.call.rpc->name;
            const bool is_trigger = name == "ServerTriggerAbility";
            const bool is_cancel = name == "ServerNotifyHeldAbilityCancelled";
            if (!is_trigger && !is_cancel) {
                ++other;
                continue;
            }
            if (rpc.on_actor || rpc.object.ObjectId != cap::kAbilityObjectId) all_read = false;
            if (!rpc.call.decoded) {
                ++undecoded;
                continue;
            }
            // written back: the parameters re-encode to the client's own bits
            BunchWriter w;
            bool failed = false;
            const size_t bits = write_rpc_params(*rpc.call.rpc, rpc.call.params, w, client_options(), &failed);
            if (inline_targets) {
                const schema::RepCommand* tc = rpc_command(*rpc.call.rpc, "Targets");
                const auto tv = tc ? rpc.call.params.values.find(tc->handle) : rpc.call.params.values.end();
                const ArrayValue* arr = tv != rpc.call.params.values.end() ? tv->second.get_if<ArrayValue>() : nullptr;
                if (arr) {
                    for (const ElementState& e : arr->elements) {
                        const auto it = e.values.find(1);
                        if (it != e.values.end() && it->second.get_if<InlineObject>()) ++*inline_targets;
                    }
                }
            }
            const auto original = slice_bits(bytes, rpc.call.param_pos, rpc.call.param_bits);
            if (!failed && bits == rpc.call.param_bits && std::equal(original.begin(), original.end(), w.data())) ++rewritten;
            Call c;
            c.is_trigger = is_trigger;
            if (is_trigger) {
                const auto t = parse_trigger_ability(rpc.call);
                if (!t) { ++undecoded; continue; }
                c.trigger = *t;
                ++triggers;
            } else {
                const auto h = parse_held_ability_cancelled(rpc.call);
                if (!h) { ++undecoded; continue; }
                c.cancel = *h;
                ++cancels;
            }
            calls.push_back(c);
        }
    }
    std::printf("%s\n", label);
    const std::string head = std::string(label) + ": ";
    CHECK(all_read, (head + "every bunch reads, every call is on the AbilityComponent subobject").c_str());
    CHECK(undecoded == 0 && triggers == want_triggers && cancels == want_cancels && expected == want_triggers + want_cancels,
          (head + std::to_string(triggers) + " ServerTriggerAbility and " + std::to_string(cancels) +
           " ServerNotifyHeldAbilityCancelled, every one decoded to its last bit").c_str());
    CHECK(rewritten == triggers + cancels, (head + "every call writes back to the client's own bits").c_str());
    (void)other;
    return calls;
}

static void check_calls(const char* label, const std::vector<Call>& calls) {
    const std::string head = std::string(label) + ": ";
    int32_t next_id = 0;
    bool contiguous = true, item_none = true, no_pvp = true, abilities_known = true, cancels_pair = true;
    std::map<int32_t, int64_t> ability_of;   // ClientInstanceId -> AbilityGuid
    std::set<int64_t> known = {kSprint, kDodge, kScepterCombo, kQuickWit, kGuard};
    size_t sprint = 0, dodge = 0;
    for (const Call& c : calls) {
        if (c.is_trigger) {
            const TriggerAbility& t = c.trigger;
            if (t.client_instance_id != next_id++) contiguous = false;
            ability_of[t.client_instance_id] = t.ability_guid;
            item_none = item_none && t.item_slot == -1 && t.item_location == 24;
            no_pvp = no_pvp && !t.pvp_force;
            abilities_known = abilities_known && known.count(t.ability_guid) == 1;
            if (t.ability_guid == kSprint) ++sprint;
            if (t.ability_guid == kDodge) ++dodge;
        } else {
            const auto it = ability_of.find(c.cancel.client_instance_id);
            // a cancel releases a held ability: Sprint or Guard in all 58 of the research's
            cancels_pair = cancels_pair && it != ability_of.end() && (it->second == kSprint || it->second == kGuard) &&
                           c.cancel.cancel_section_index == -1;
        }
    }
    CHECK(contiguous, (head + "ClientInstanceId counts every trigger of any ability from 0 without a gap").c_str());
    CHECK(item_none && no_pvp, (head + "ItemData is \"no item\" (Slot -1, Location 24) and bPvpForceFlag is 0 in every trigger").c_str());
    CHECK(abilities_known, (head + "every AbilityGuid is Sprint, Dodge, the Scepter combo, QuickWit or Guard").c_str());
    CHECK(cancels_pair, (head + "every cancel names the ClientInstanceId of an earlier Sprint or Guard, section -1").c_str());
    CHECK(sprint > 0, (head + std::to_string(sprint) + " Sprints and " + std::to_string(dodge) + " Dodges").c_str());
}

static void test_inline_object() {
    std::printf("an object named in place (InlineObject)\n");
    // The pawn's CollisionCylinder as the e2e capture's Scepter combos name it: flags 5 (path + network checksum), the pawn as
    // the outer, the name, the checksum 0xb876932f.
    InlineObject cylinder;
    cylinder.flags = 5;
    cylinder.outer = FIntrepidNetworkGUID{10341538, 60, 0x6ee87ee4};
    cylinder.path = "CollisionCylinder";
    cylinder.checksum = 0xb876932f;
    BunchWriter w;
    CHECK(write_inline_object(w, cylinder) && w.bit_pos() == 128 + 8 + 128 + 32 + 18 * 8 + 32, "written: the GUID 1, flags, outer, the FString, the checksum");
    PacketReader r(w.data(), w.byte_size(), w.bit_pos());
    const auto back = read_object_value(r, true);
    CHECK(back && back->get_if<InlineObject>() && *back->get_if<InlineObject>() == cylinder && r.pos() == w.bit_pos(), "reads back equal");
    PacketReader refused(w.data(), w.byte_size(), w.bit_pos());
    CHECK(!read_object_value(refused, false).has_value(), "refused where a parameter may not be one (a server's property never is)");
    BunchWriter plain;
    emit::write_intrepid_guid(plain, FIntrepidNetworkGUID{10341558, 60, 0});
    PacketReader rp(plain.data(), plain.byte_size(), plain.bit_pos());
    const auto g = read_object_value(rp, true);
    CHECK(g && g->get_if<FIntrepidNetworkGUID>() && g->get_if<FIntrepidNetworkGUID>()->ObjectId == 10341558, "a plain reference is still a plain reference");

    InlineObject bad = cylinder;
    BunchWriter sink;
    bad.flags = 8;
    CHECK(!write_inline_object(sink, bad) && sink.bit_pos() == 0, "flags beyond path, no-load and checksum are refused");
    bad = cylinder;
    bad.path.clear();
    CHECK(!write_inline_object(sink, bad), "a path flag with no name is refused");
    bad = cylinder;
    bad.outer.ObjectId = 1;
    CHECK(!write_inline_object(sink, bad), "an outer that is itself inline is refused");
    bad = cylinder;
    bad.flags = 4;
    CHECK(!write_inline_object(sink, bad), "a checksum without a path is refused (UE reads it with the path only)");
    // A damaged export: the checksum cut off.
    PacketReader cut(w.data(), w.byte_size(), w.bit_pos() - 16);
    CHECK(!read_object_value(cut, true).has_value(), "a read past the end is refused");
    // Export flags the form does not define are refused on the way in, not only on the way out: the flags byte follows the 16 bytes of
    // the GUID.  A complete export with 8 or 0x80 added to flags 5, and flags 8 and 0x80 alone, which no path follows.
    for (const uint8_t flags : {static_cast<uint8_t>(5 | 8), static_cast<uint8_t>(5 | 0x80), static_cast<uint8_t>(5 | 0xf8)}) {
        std::vector<uint8_t> bytes(w.data(), w.data() + w.byte_size());
        bytes[16] = flags;
        PacketReader odd(bytes.data(), bytes.size(), w.bit_pos());
        CHECK(!read_object_value(odd, true).has_value(), ("an export with flags " + std::to_string(static_cast<int>(flags)) + " (bits the form does not define) is refused").c_str());
    }
    for (const uint8_t flags : {static_cast<uint8_t>(8), static_cast<uint8_t>(0x80), static_cast<uint8_t>(0xff)}) {
        BunchWriter bare;
        FIntrepidNetworkGUID root;
        root.ObjectId = 1;
        emit::write_intrepid_guid(bare, root);
        bare.write_uint8(flags);
        PacketReader rb(bare.data(), bare.byte_size(), bare.bit_pos());
        CHECK(!read_object_value(rb, true).has_value(), ("flags " + std::to_string(static_cast<int>(flags)) + " and nothing after them are refused").c_str());
    }
    BunchWriter plain_flags;   // the one pathless form that is fine: flags 0, no outer, no name
    FIntrepidNetworkGUID root;
    root.ObjectId = 1;
    emit::write_intrepid_guid(plain_flags, root);
    plain_flags.write_uint8(0);
    PacketReader rf(plain_flags.data(), plain_flags.byte_size(), plain_flags.bit_pos());
    const auto bare_inline = read_object_value(rf, true);
    CHECK(bare_inline && bare_inline->get_if<InlineObject>() && bare_inline->get_if<InlineObject>()->flags == 0, "(flags 0 alone is an inline object with no path)");
}

static void test_captured_calls() {
    std::printf("the captured calls: read to their last bit, written back\n");
    size_t inline_targets = 0;
    const auto e2e = read_capture("the e2e capture", cap::kE2e, std::size(cap::kE2e), cap::kE2eTriggers, cap::kE2eCancels, &inline_targets);
    CHECK(inline_targets == 8, "8 of the e2e capture's triggers (the Scepter combos) name their target in place: ObjectId 1 and the export");
    // plan E1, stage 3 fix: a target named in place is a target (the client names an NPC's collision cylinder so, the server never having
    // exported it); parse_trigger_ability keeps it beside the plain GUIDs, and the two together are the call's Targets count
    size_t parsed_inline = 0;
    bool counts_match = true;
    for (const Call& c : e2e) {
        if (!c.is_trigger) continue;
        parsed_inline += c.trigger.inline_targets.size();
        counts_match = counts_match && c.trigger.target_count == c.trigger.targets.size() + c.trigger.inline_targets.size();
        for (const InlineObject& o : c.trigger.inline_targets) counts_match = counts_match && o.path == "CollisionCylinder" && o.outer.ObjectId != 0;
    }
    CHECK(parsed_inline == 8 && counts_match, "parse_trigger_ability keeps the 8 targets named in place (the pawn's CollisionCylinder, its outer the pawn), and every call's Targets count is its GUIDs plus its inline objects");
    const auto native = read_capture("the 2026-09-29 native run", cap::kNative, std::size(cap::kNative), cap::kNativeTriggers, cap::kNativeCancels);
    check_calls("the e2e capture", e2e);
    check_calls("the native run", native);

    // The native run's own numbers (research): 4 Sprints (ids 0-3), 24 Dodges (4-27, AnimationIndex 5, input (1, 0)), 3 Scepter
    // combos (28-30); the cancels release Sprints 0, 1 and 2.
    size_t sprints = 0, dodges = 0, forward_dodges = 0, combos = 0;
    std::vector<int32_t> cancelled;
    for (const Call& c : native) {
        if (!c.is_trigger) {
            cancelled.push_back(c.cancel.client_instance_id);
            continue;
        }
        sprints += c.trigger.ability_guid == kSprint;
        dodges += c.trigger.ability_guid == kDodge;
        combos += c.trigger.ability_guid == kScepterCombo;
        forward_dodges += c.trigger.ability_guid == kDodge && c.trigger.animation_index == 5 && c.trigger.input_x == 1 && c.trigger.input_y == 0;
    }
    CHECK(sprints == 4 && dodges == 24 && combos == 3 && forward_dodges == 24, "the native run: 4 Sprints, 24 forward Dodges (AnimationIndex 5, input (1, 0)), 3 Scepter combos");
    CHECK(cancelled == std::vector<int32_t>({0, 1, 2}), "its three cancels release instances 0, 1 and 2 (Sprint 3 has none in the capture)");
    CHECK(!native.empty() && native[0].is_trigger && native[0].trigger.ability_guid == kSprint && native[0].trigger.client_instance_id == 0 &&
              native[0].trigger.animation_index == 0 && native[0].trigger.target_count == 0 && native[0].trigger.target_transform_count == 0,
          "the first call is the first Sprint: ClientInstanceId omitted (0), AnimationIndex omitted, no targets");
}
// (C | Y | B + C O A)

static void test_parse_helpers() {
    std::printf("the parse helpers\n");
    const cap::Bunch& b = cap::kE2e[0];
    const auto bytes = bytes_of_hex(b.hex);
    const auto parsed = read_client_bunch(pawn(), resolve_ability, bytes.data(), b.bits, client_options());
    const ClientRpc* trigger = nullptr;
    if (parsed) {   // (the range is the parsed bunch's own: a pointer into a temporary copy would dangle after the loop)
        for (const ClientRpc& r : parsed->rpcs) {
            if (std::string(r.call.rpc->name) == "ServerTriggerAbility") trigger = &r;
        }
    }
    CHECK(trigger != nullptr, "the first captured bunch holds a ServerTriggerAbility");
    if (!trigger) return;
    const auto t = parse_trigger_ability(trigger->call);
    CHECK(t && t->ability_guid == kSprint && t->input_x == 1 && t->input_y == 0 && t->item_slot == -1 && t->item_location == 24,
          "its Sprint, input (1, 0), no item");
    CHECK(!parse_held_ability_cancelled(trigger->call), "it is not a ServerNotifyHeldAbilityCancelled");
    RpcCall undecoded = trigger->call;
    undecoded.decoded = false;
    CHECK(!parse_trigger_ability(undecoded), "a call whose parameters did not decode is not acted on");
    RpcCall none;
    CHECK(!parse_trigger_ability(none) && !parse_held_ability_cancelled(none), "an empty call parses as nothing");
    for (size_t i = 0; i < std::size(cap::kE2e); ++i) {
        if (cap::kE2e[i].cancels == 0) continue;
        const auto cb = bytes_of_hex(cap::kE2e[i].hex);
        const auto cp = read_client_bunch(pawn(), resolve_ability, cb.data(), cap::kE2e[i].bits, client_options());
        bool seen = false;
        if (!cp) continue;
        for (const ClientRpc& r : cp->rpcs) {
            const auto h = parse_held_ability_cancelled(r.call);
            if (!h) continue;
            seen = true;
            CHECK(h->cancel_section_index == -1 && h->cancelled_time >= 0 && h->cancelled_time < 2 && !parse_trigger_ability(r.call),
                  "a cancel: CancelSectionIndex -1, CancelledTime a fraction of a second");
        }
        if (seen) break;
    }
}

static void test_client_end_ability() {
    std::printf("ClientEndAbility\n");
    Actor pawn_actor;
    pawn_actor.guid = FIntrepidNetworkGUID{10341538, 60, 0};
    pawn_actor.obj.schema = &pawn();
    Subobject comp;
    comp.guid = FIntrepidNetworkGUID{cap::kAbilityObjectId, 60, 0};
    comp.name = "AbilityComponent";
    comp.checksum = 0x9e63ae5c;
    comp.obj.schema = &ability();
    pawn_actor.subobjects.push_back(comp);
    ExportAcks acks;
    for (const int32_t id : {0, 7, 30}) {
        const auto e = client_end_ability(pawn_actor, pawn_actor.subobjects[0], id, acks);
        CHECK(e && e->reliable, "ClientEndAbility is a reliable RPC (function flags 0x01020cc0)");
        if (!e) continue;
        // The block after the component's export: the server's form, a stably named subobject.
        const auto blocks = read_content_blocks(e->payload.data(), e->payload.bit_pos(), 0, true);
        CHECK(blocks && blocks->size() == 1 && !(*blocks)[0].header.is_actor && (*blocks)[0].header.object == comp.guid, "one block, on the AbilityComponent");
        if (!blocks || blocks->empty()) continue;
        const BlockView& v = (*blocks)[0];
        size_t pos = v.payload_pos;
        const auto calls = read_rpc_fields(ability(), e->payload.data(), v.payload_pos + v.payload_bits, pos);
        CHECK(calls && calls->size() == 1 && (*calls)[0].decoded && std::string((*calls)[0].rpc->name) == "ClientEndAbility" &&
                  (id == 0 ? (*calls)[0].params.values.count(1) == 0   // 0 is the default: the send bit is 0
                           : (*calls)[0].params.values.count(1) && (*calls)[0].params.values.at(1) == Value::i64(id)),
              ("ClientEndAbility(ClientInstanceId = " + std::to_string(id) + ") reads back").c_str());
    }
    Subobject bare;
    bare.guid = FIntrepidNetworkGUID{5, 60, 0};
    bare.obj.schema = &pawn();   // a class without the RPC
    pawn_actor.subobjects.push_back(bare);
    CHECK(!client_end_ability(pawn_actor, pawn_actor.subobjects[1], 1, acks), "a component whose class has no ClientEndAbility gets nothing");
}
// ~ CoA :: C-Y-B ~

// A seeded fuzz of what a hostile client can send: every captured bunch with bits flipped, bits cut off and its tail replaced by
// noise, each read from a heap buffer of exactly the bytes it needs (an address sanitizer build reports an over-read).  Nothing
// may crash or hang; whatever decodes as a trigger or a cancel parses to the same values from the bits it writes back.
static uint64_t g_fuzz_state = 0x9e3779b97f4a7c15ull;
static uint64_t fuzz_next() {
    g_fuzz_state ^= g_fuzz_state << 13;
    g_fuzz_state ^= g_fuzz_state >> 7;
    g_fuzz_state ^= g_fuzz_state << 17;
    return g_fuzz_state;
}

/// Equal as bit patterns: a mutated bunch can carry a NaN, which never equals itself.
template <class T>
static bool same_bits(T a, T b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }

static void test_fuzz() {
    std::printf("hostile bunches\n");
    size_t reads = 0, decoded = 0, rewritten_same = 0, rewritten = 0;
    const auto run = [&](const std::vector<uint8_t>& bytes, size_t bits) {
        std::unique_ptr<uint8_t[]> exact(new uint8_t[bytes.size() ? bytes.size() : 1]);
        std::copy(bytes.begin(), bytes.end(), exact.get());
        ++reads;
        const auto parsed = read_client_bunch(pawn(), resolve_ability, exact.get(), bits, client_options());
        if (!parsed) return;
        for (const ClientRpc& rpc : parsed->rpcs) {
            const auto t = parse_trigger_ability(rpc.call);
            const auto c = parse_held_ability_cancelled(rpc.call);
            if (!t && !c) continue;
            ++decoded;
            BunchWriter w;
            bool failed = false;
            const size_t n = write_rpc_params(*rpc.call.rpc, rpc.call.params, w, client_options(), &failed);
            if (failed || n == 0) continue;   // a value the writer refuses (a non-minimal form): fine, nothing was written
            ++rewritten;
            // what was written reads back to the same parameters
            size_t pos = 0;
            const auto again = read_rpc_params(*rpc.call.rpc, w.data(), n, pos, client_options());
            if (!again) continue;
            RpcCall back = rpc.call;
            back.params = *again;
            const auto t2 = parse_trigger_ability(back);
            const auto c2 = parse_held_ability_cancelled(back);
            const bool same_trigger = t && t2 && t->ability_guid == t2->ability_guid && t->client_instance_id == t2->client_instance_id &&
                                      t->animation_index == t2->animation_index && same_bits(t->input_x, t2->input_x) && same_bits(t->input_y, t2->input_y) &&
                                      t->pvp_force == t2->pvp_force && t->target_count == t2->target_count &&
                                      t->target_transform_count == t2->target_transform_count && t->item_slot == t2->item_slot &&
                                      t->item_location == t2->item_location;
            const bool same_cancel = c && c2 && c->client_instance_id == c2->client_instance_id && same_bits(c->cancelled_time, c2->cancelled_time) &&
                                     c->cancel_section_index == c2->cancel_section_index;
            if (same_trigger || same_cancel) ++rewritten_same;
        }
    };
    const auto each = [&](auto&& fn) {
        for (const cap::Bunch* set : {static_cast<const cap::Bunch*>(cap::kE2e), static_cast<const cap::Bunch*>(cap::kNative)}) {
            const size_t n = set == cap::kE2e ? std::size(cap::kE2e) : std::size(cap::kNative);
            for (size_t i = 0; i < n; ++i) fn(bytes_of_hex(set[i].hex), static_cast<size_t>(set[i].bits));
        }
    };
    each([&](std::vector<uint8_t> bytes, size_t bits) {
        run(bytes, bits);
        for (int round = 0; round < 200; ++round) {
            std::vector<uint8_t> m = bytes;
            const int flips = 1 + static_cast<int>(fuzz_next() % 4);
            for (int k = 0; k < flips; ++k) {
                const size_t bit = fuzz_next() % bits;
                m[bit >> 3] ^= static_cast<uint8_t>(1u << (bit & 7));
            }
            run(m, bits);
        }
        for (int round = 0; round < 60; ++round) {   // cut short: fewer bits, and the buffer exactly that long
            const size_t cut = 1 + fuzz_next() % bits;
            std::vector<uint8_t> m(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>((cut + 7) / 8));
            run(m, cut);
        }
        for (int round = 0; round < 40; ++round) {   // the tail replaced by noise
            std::vector<uint8_t> m = bytes;
            const size_t from = fuzz_next() % bits;
            for (size_t bit = from; bit < bits; ++bit) {
                if (fuzz_next() & 1) m[bit >> 3] ^= static_cast<uint8_t>(1u << (bit & 7));
            }
            run(m, bits);
        }
    });
    std::printf("    %zu reads, %zu of them a trigger or a cancel, %zu of those written back, %zu reading back equal\n", reads, decoded, rewritten, rewritten_same);
    CHECK(reads > 16000 && decoded > 0 && rewritten_same == rewritten,
          "16,254 mutated bunches: none crashes or hangs, and every trigger or cancel that decodes reads back equal from the bits it writes");
}

int main() {
    test_item_instance_codec();
    test_inline_object();
    test_captured_calls();
    test_parse_helpers();
    test_client_end_ability();
    test_fuzz();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
