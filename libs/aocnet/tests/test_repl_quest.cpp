// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_quest.cpp
//
//  Quests, slice 1: the quest component's wire against what the retail client received and sent.
//
//    * The codecs (protocol/repl/quest_codec.h): an empty reward is six zero words, a reward with an item and a variable list round trip, a map or an id list that is not empty is refused.
//    * The component's schema (UQuestConsumerComponent): its seven fast arrays, and the item layout the quest cache has (the quest instance's own members first, its guid and record last).
//    * Against the recordings (AOC_RECORDINGS_DIR, quest_blocks.txt: skipped without): the block a quest accept put on the wire (the cache item, the objective marker and the variables
//      of the new quest) read to its last bit, every field written again from what was read: the recorded bits; the queue of the offer panel (QueuedSubDialogues, with its
//      QuestPreview) read and written again; the emptied queue; the accept and abandon calls; the abandon's two deletions; and a commission's larger block.
//
//  The recorded blocks are one line each, "label dir rep bits hex", in the folder AOC_RECORDINGS_DIR names; without them the recorded part prints SKIP.
// ============================================================================
#include "recordings.h"
#include "test_support.h"

#include "protocol/repl/dialogue_rpc.h"
#include "protocol/repl/fast_array.h"
#include "protocol/repl/quest_codec.h"
#include "protocol/repl/rep_block.h"
#include "protocol/repl/rpc.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using namespace tsupport;
using emit::BunchWriter;

namespace {

struct Recorded {
    std::string dir;
    bool        rep = false;
    size_t      bits = 0;
    Bits        payload;
};

std::map<std::string, Recorded> load(const std::string& path) {
    std::map<std::string, Recorded> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string label, dir, hex;
        int rep = 0;
        size_t bits = 0;
        if (!(ss >> label >> dir >> rep >> bits >> hex)) continue;
        Recorded r;
        r.dir = dir;
        r.rep = rep != 0;
        r.bits = bits;
        r.payload = bits_of_hex(hex.c_str(), bits);
        out[label] = std::move(r);
    }
    return out;
}

const schema::ClassSchema& quest_schema() {
    const schema::ClassSchema* s = schema::gen::find_component_schema("QuestConsumerComponent");
    if (!s) { std::printf("  [FAIL] no schema for the quest component\n"); std::exit(2); }
    return *s;
}

// ---- the codecs -------------------------------------------------------------------------------------------------------------------

void test_codecs() {
    std::printf("-- the reward and variable codecs\n");
    {   // an empty reward: six zero words
        RewardInstance empty;
        BunchWriter w;
        CHECK(write_reward_instance(w, empty) && w.bit_pos() == 192, "an empty reward is 192 bits");
        const Bits bits(w);
        bool all_zero = true;
        for (size_t i = 0; i < bits.size(); ++i) all_zero = all_zero && !bits.b[i];
        CHECK(all_zero, "... all of them zero");
        const auto back = with_exact(bits, [&](const uint8_t* d, size_t bytes, size_t n) {
            wire::PacketReader r(d, bytes, n);
            return read_reward_instance(r);
        });
        CHECK(back && *back == empty, "it reads back");
    }
    {   // a reward with an item and experience
        RewardInstance rw;
        ItemInstance item;
        item.has_item = true;
        item.record_id = 6064632204386435184LL;
        item.quantity = 3;
        item.quality = 1;
        item.guid = {1, 2, 3, 4};
        item.creation_time = 639049760260000000LL;
        rw.items.push_back(item);
        rw.node_activity_experience = 250;
        rw.guild_activity_experience = 7;
        BunchWriter w;
        CHECK(write_reward_instance(w, rw), "a reward with an item and two experience numbers writes");
        const Bits bits(w);
        const auto back = with_exact(bits, [&](const uint8_t* d, size_t bytes, size_t n) {
            wire::PacketReader r(d, bytes, n);
            return read_reward_instance(r);
        });
        CHECK(back && *back == rw, "it reads back, the item and both numbers");
        // a map that is not empty is refused (no capture has one)
        Bits tampered = bits;
        const size_t maps_at = 32 + 1096 + 64;   // the item count, the item, the two experience words
        tampered.set(maps_at, 1, 32);
        const auto refused = with_exact(tampered, [&](const uint8_t* d, size_t bytes, size_t n) {
            wire::PacketReader r(d, bytes, n);
            return read_reward_instance(r);
        });
        CHECK(!refused, "a reward whose experience map is not empty is refused");
    }
    {   // variables
        HashedVariable v;
        v.variables = {{0xc4b93bb9a94a4f66ULL, 0}, {0xc9f1f94610622220ULL, 1}, {0x2aabe7742bf32defULL, -2}};
        BunchWriter w;
        CHECK(write_hashed_variable(w, v) && w.bit_pos() == 32 + 3 * 96, "three variables are 32 + 3 x 96 bits");
        const Bits bits(w);
        const auto back = with_exact(bits, [&](const uint8_t* d, size_t bytes, size_t n) {
            wire::PacketReader r(d, bytes, n);
            return read_hashed_variable(r);
        });
        CHECK(back && *back == v, "they read back, a negative value too");
    }
}

// ---- the schema -------------------------------------------------------------------------------------------------------------------

void test_schema() {
    std::printf("-- the component's schema\n");
    const schema::ClassSchema& cls = quest_schema();
    for (const char* f : {"ReplicatedQuestCache", "ObjectiveMarkerCache", "RepQuestVariables", "QuestGiverInfos", "NPCCalloutAmbientInfos", "ActorGuidToActorAuthStats",
                          "QuestInstanceGuidToParticipationGroups"}) {
        const auto layout = fast_array_layout(cls, f);
        CHECK(layout.has_value(), (std::string("the fast array ") + f + " has a layout").c_str());
    }
    const auto cache = fast_array_layout(cls, "ReplicatedQuestCache");
    if (!cache) return;
    CHECK(!cache->element.empty() && std::string(cache->element.front().path) == "Quests.InstigatorItemInstance.ItemId.Guid", "a quest item starts with the instigator item");
    CHECK(std::string(cache->element[cache->element.size() - 2].path) == "Quests.InstanceGuid.D" && std::string(cache->element.back().path) == "Quests.RecordId",
          "... and ends with the quest instance's guid and record");
}

// ---- the recordings ---------------------------------------------------------------------------------------------------------------

/// Writes one array of a recorded block again from what was read of it (the state it came from, written against the shadow the delta was made against); true when its bits are the recorded ones.
void check_field_again(const Recorded& rec, const std::string& what, const schema::ClassSchema& cls, uint32_t field, const FastArrayDelta& delta) {
    const auto layout = fast_array_layout(cls, field);
    CHECK(layout.has_value(), (what + ": the array has a layout").c_str());
    if (!layout) return;
    FastArrayState st(*layout);
    for (const auto& c : delta.changed) st.insert(c.id, c.values);
    st.set_array_key(delta.array_key);
    FastArrayShadow shadow;
    shadow.array_key = delta.base_key;
    for (const int32_t id : delta.deleted) shadow.items.push_back({id, 0});
    BunchWriter w;
    bool failed = false;
    const size_t n = write_fast_array_delta(st, shadow, w, {}, &failed);
    CHECK(!failed && n > 0, (what + ": field " + std::to_string(field) + " writes again").c_str());
    // the recorded field: its header (field index, bit count) and then the payload; compare the payloads
    const Bits mine(w);
    bool found = false;
    for (size_t start = 0; start + mine.size() <= rec.payload.size() && !found; ++start) {
        if (rec.payload.get(start, 8) != mine.get(0, 8)) continue;
        found = rec.payload.slice(start, mine.size()) == mine;
    }
    CHECK(found, (what + ": field " + std::to_string(field) + " written again (" + std::to_string(mine.size()) + " bits) is in the recorded block bit for bit").c_str());
}

/// Reads the recorded block's fast array fields and writes every one again from what was read.
void check_component_block(const Recorded& rec, const std::string& what, size_t expect_fields) {
    const schema::ClassSchema& cls = quest_schema();
    const auto fields = with_exact(rec.payload, [&](const uint8_t* d, size_t, size_t n) -> std::optional<std::vector<FastArrayField>> {
        size_t pos = 0;
        return read_fast_array_fields(cls, d, n, pos, {});
    });
    CHECK(fields.has_value(), (what + ": the block reads to its last bit as fast arrays").c_str());
    if (!fields) return;
    CHECK(fields->size() == expect_fields, (what + ": " + std::to_string(expect_fields) + " arrays").c_str());
    for (const FastArrayField& f : *fields) check_field_again(rec, what, cls, f.field, f.delta);
}

// The structure of a quest cache item that has a reward the codec does not read: where the item starts and ends comes from the field header and the delta header alone.  Its front is the
// instigator item (the item id's type id is the 64 bits after the first 64) and its back is the quest's InstanceGuid (128 bits) and RecordId (64 bits), as the layout says.
struct ItemProbe {
    bool     found = false;
    size_t   item_bits = 0;
    uint64_t type_id = 0, record = 0;
    uint32_t guid_or = 0;
};
ItemProbe probe_cache_item(const Bits& payload) {
    const schema::ClassSchema& cls = quest_schema();
    const auto layout = fast_array_layout(cls, "ReplicatedQuestCache");
    if (!layout) return ItemProbe{};
    return with_exact(payload, [&](const uint8_t* d, size_t bytes, size_t n) {
        ItemProbe pr;
        size_t pos = 0;
        while (pos < n) {
            const auto h = read_field_header(cls, d, n, pos);
            if (!h) return pr;
            const size_t end = pos + h->payload_bits;
            if (h->field == layout->field) {
                wire::PacketReader r(d, bytes, end);
                r.set_pos(pos);
                r.read_bits(1);                       // bSupportsFastArrayDelta
                r.read_bits(32);                      // ArrayReplicationKey
                r.read_bits(32);                      // BaseReplicationKey
                const uint32_t deleted = static_cast<uint32_t>(r.read_bits(32));
                const uint32_t changed = static_cast<uint32_t>(r.read_bits(32));
                for (uint32_t i = 0; i < deleted; ++i) r.read_bits(32);
                if (changed != 1) return pr;
                r.read_bits(32);                      // ElementID
                const size_t item_at = r.pos();
                pr.item_bits = end - item_at;
                if (pr.item_bits < 64 + 64 + 192) return pr;
                r.read_bits(64);                      // InstigatorItemInstance.ItemId.Guid
                pr.type_id = r.read_bits(64);         // ... .TypeId
                r.set_pos(end - 192);
                for (int i = 0; i < 4; ++i) pr.guid_or |= static_cast<uint32_t>(r.read_bits(32));
                pr.record = r.read_bits(64);
                pr.found = true;
                return pr;
            }
            pos = end;
        }
        return pr;
    });
}

// How far does a recorded cache item read?  Reads the item command by command (as read_element does), which says where it stops: the quests with a reward the codec does not read
// stop at the reward.
struct CachePrefix {
    bool        found = false;
    size_t      items = 0, objectives = 0, bits_before_reward = 0, bits_after_reward = 0;
    std::string stopped_at;   // the member that did not read ("" : the whole item read)
};
CachePrefix read_cache_prefix(const Bits& payload) {
    CachePrefix out;
    const schema::ClassSchema& cls = quest_schema();
    const auto layout = fast_array_layout(cls, "ReplicatedQuestCache");
    if (!layout) return out;
    with_exact(payload, [&](const uint8_t* d, size_t bytes, size_t n) {
        size_t pos = 0;
        while (pos < n) {
            const auto h = read_field_header(cls, d, n, pos);
            if (!h) return 0;
            const size_t end = pos + h->payload_bits;
            if (h->field == layout->field) {
                wire::PacketReader r(d, bytes, end);
                r.set_pos(pos);
                r.read_bits(1);
                r.read_bits(32);
                r.read_bits(32);
                const uint32_t deleted = static_cast<uint32_t>(r.read_bits(32)), changed = static_cast<uint32_t>(r.read_bits(32));
                for (uint32_t i = 0; i < deleted; ++i) r.read_bits(32);
                out.found = true;
                for (uint32_t it = 0; it < changed; ++it) {
                    r.read_bits(32);   // ElementID
                    const size_t item_at = r.pos();
                    for (const schema::RepCommand& c : layout->element) {
                        const size_t at = r.pos();
                        bool ok = true;
                        if (c.type == schema::PropType::DynamicArray) {
                            const size_t count = static_cast<size_t>(r.read_bits(16));
                            const auto sub = layout->table.subspan(c.elem_begin, c.elem_end - c.elem_begin);
                            for (size_t k = 0; k < count && ok; ++k) ok = read_element(sub, layout->table, r, {}).has_value();
                            if (ok && std::string(c.path) == "Quests.Objectives") out.objectives = count;
                        } else {
                            ok = read_value(c, layout->table, r, {}).has_value();
                        }
                        if (!ok) {
                            out.stopped_at = c.path;
                            out.bits_before_reward = at - item_at;
                            out.bits_after_reward = end - at;
                            return 0;
                        }
                    }
                    ++out.items;
                }
            }
            pos = end;
        }
        return 0;
    });
    return out;
}

/// A recorded block whose quest cache holds a reward the codec does not read (a quest that pays an item): each array on its own.  The marker and variable arrays are read and written
/// again to the recorded bits; the cache item is read as far as the reward, and its front and back are where the layout puts them.
void check_block_by_array(const Recorded& rec, const std::string& what, size_t expect_fields, size_t objectives) {
    const schema::ClassSchema& cls = quest_schema();
    const auto cache = fast_array_layout(cls, "ReplicatedQuestCache");
    size_t seen = 0;
    with_exact(rec.payload, [&](const uint8_t* d, size_t, size_t n) {
        size_t pos = 0;
        while (pos < n) {
            const auto h = read_field_header(cls, d, n, pos);
            CHECK(h.has_value(), (what + ": a field header reads").c_str());
            if (!h) break;
            const size_t end = pos + h->payload_bits;
            ++seen;
            const auto layout = fast_array_layout(cls, h->field);
            CHECK(layout.has_value(), (what + ": the array has a layout").c_str());
            if (layout && cache && layout->field != cache->field) {
                size_t p = pos;
                const auto delta = read_fast_array_delta(*layout, d, end, p, {});
                CHECK(delta.has_value() && p == end, (what + ": field " + std::to_string(h->field) + " reads to its last bit").c_str());
                if (delta && p == end) check_field_again(rec, what, cls, h->field, *delta);
            }
            pos = end;
        }
        return 0;
    });
    CHECK(seen == expect_fields, (what + ": " + std::to_string(expect_fields) + " arrays").c_str());
    const CachePrefix prefix = read_cache_prefix(rec.payload);
    CHECK(prefix.found && prefix.stopped_at == "Quests.RewardInstance" && prefix.objectives == objectives,
          (what + ": the quest cache item reads through its " + std::to_string(objectives) + " objectives and their lists, and stops at the reward (" + std::to_string(prefix.bits_before_reward) + " bits in, " +
           std::to_string(prefix.bits_after_reward) + " left)").c_str());
    const ItemProbe probe = probe_cache_item(rec.payload);
    std::printf("  [info] %s: the item is %zu bits, its instigator item's type id is %llu and it ends with the record %llu\n", what.c_str(), probe.item_bits, static_cast<unsigned long long>(probe.type_id),
                static_cast<unsigned long long>(probe.record));
    CHECK(probe.found && probe.type_id == 2616523457551378613ULL && probe.guid_or != 0 && (probe.record >> 48) == 0x5429u,
          (what + ": the item's front is the instigator item's type id, its back an instance guid and a record id of the design data").c_str());
}

void test_recorded() {
    std::printf("-- the quest blocks as recorded\n");
    const std::string path = recordings::find("quest_blocks.txt");
    if (path.empty()) { recordings::print_skip("quest_blocks.txt"); return; }
    const auto rec = load(path);
    const auto get = [&](const char* label) -> const Recorded* {
        const auto it = rec.find(label);
        if (it == rec.end()) { CHECK(false, (std::string("the recording has ") + label).c_str()); return nullptr; }
        return &it->second;
    };
    if (const Recorded* r = get("accept_component")) check_component_block(*r, "an accept's component block", 3);
    if (const Recorded* r = get("abandon_component")) check_component_block(*r, "an abandon's component block", 2);
    if (const Recorded* r = get("abandon_component_2")) check_component_block(*r, "a second abandon's component block", 2);
    if (const Recorded* r = get("login_component")) check_block_by_array(*r, "a new character's login block (24,361 bits: Welcome to Verra, its 32 objective markers, its 67 variables)", 3, 9);

    // the offer panel: QueuedSubDialogues as one RepLayout command
    const schema::ClassSchema& pc = schema_named("AoCPlayerControllerBP_C");
    const auto read_pc = [&](const Recorded& r) {
        return with_exact(r.payload, [&](const uint8_t* d, size_t, size_t n) -> std::optional<RepState> {
            size_t pos = 0;
            return read_rep_block(pc, d, n, pos);
        });
    };
    const schema::RepCommand* queued = pc.command("CurrentDialogueInstance.QueuedSubDialogues");
    CHECK(queued != nullptr, "the controller has the queue of offers");
    if (const Recorded* r = get("offer_item_quest")) {
        const auto st = read_pc(*r);
        CHECK(st.has_value(), "the offer panel's block reads to its last bit");
        if (st && queued) {
            const Value* v = st->get(queued->handle);
            const ArrayValue* a = v ? v->get_if<ArrayValue>() : nullptr;
            CHECK(a && a->elements.size() == 1, "one offer is queued");
            BunchWriter w;
            CHECK(v && write_value(*queued, pc.element_commands, *v, w, {}), "the queue writes again");
            // the recorded block: bDoChecksum, the handle, then the value
            const Bits mine(w);
            bool found = false;
            for (size_t start = 0; start + mine.size() <= r->payload.size() && !found; ++start) found = r->payload.slice(start, mine.size()) == mine;
            CHECK(found, ("the queue written again (" + std::to_string(mine.size()) + " bits) is in the recorded block bit for bit").c_str());
            if (a && !a->elements.empty()) {
                const auto& e = a->elements[0].values;
                const auto num = [&](uint16_t h) -> int64_t { const auto it = e.find(h); const auto* i = it == e.end() ? nullptr : it->second.get_if<int64_t>(); return i ? *i : -1; };
                const auto kind = e.find(1);
                CHECK(kind != e.end() && kind->second.get_if<uint64_t>() && *kind->second.get_if<uint64_t>() == 2, "its type is QuestAccept (2)");
                CHECK(num(15) > 0 && num(26) == num(15), "the preview names the quest the offer names");
            }
        }
    }
    if (const Recorded* r = get("accept_cleared")) {
        const auto st = read_pc(*r);
        CHECK(st.has_value(), "the emptied queue's block reads");
        const schema::RepCommand* exhaust = pc.command("CurrentDialogueInstance.ExhaustBehavior");
        if (st && queued && exhaust) {
            const Value* q = st->get(queued->handle);
            const ArrayValue* a = q ? q->get_if<ArrayValue>() : nullptr;
            CHECK(a && a->elements.empty(), "the queue is empty");
            BunchWriter w;
            CHECK(q && write_value(*queued, pc.element_commands, *q, w, {}) && w.bit_pos() == 24, "an empty queue is 24 bits");
            const Value* x = st->get(exhaust->handle);
            CHECK(x != nullptr, "ExhaustBehavior is written with it");
        }
    }

    // the client's calls
    const auto call_of = [&](const Recorded& r, const char* rpc_name) -> std::optional<RpcCall> {
        const auto calls = with_exact(r.payload, [&](const uint8_t* d, size_t, size_t n) -> std::optional<std::vector<RpcCall>> {
            size_t pos = 0;
            return read_rpc_fields(pc, d, n, pos, {});
        });
        if (!calls || calls->empty()) return std::nullopt;
        for (const RpcCall& c : *calls) {
            if (c.rpc && std::string(c.rpc->name) == rpc_name && c.decoded) return c;
        }
        return std::nullopt;
    };
    if (const Recorded* r = get("accept_response")) {
        const auto c = call_of(*r, "ServerSelectDialogueChoice");
        CHECK(c.has_value(), "the accept is a ServerSelectDialogueChoice that reads to its last bit");
        const auto parsed = c ? parse_dialogue_response(*c) : std::nullopt;
        CHECK(parsed && parsed->response_type == 2 && parsed->quest_response == 1 && c->param_bits == 184, "it is the quest panel's Accept: 184 bits");
    }
    for (const char* label : {"abandon_request", "abandon_request_2"}) {
        if (const Recorded* r = get(label)) {
            const auto c = call_of(*r, "ServerAbandonQuests");
            CHECK(c.has_value(), (std::string(label) + " is a ServerAbandonQuests that reads to its last bit").c_str());
            if (!c) continue;
            const auto* guids = c->params.values.count(1) ? c->params.values.at(1).get_if<ArrayValue>() : nullptr;
            const auto state = c->params.values.count(2) ? c->params.values.at(2).get_if<uint64_t>() : nullptr;
            CHECK(guids && guids->elements.size() == 1 && state && *state == 7, "it names one quest instance and the state Abandoned (7)");
        }
    }
    if (const Recorded* r = get("commission_component")) {
        const schema::ClassSchema& cls = quest_schema();
        const auto fields = with_exact(r->payload, [&](const uint8_t* d, size_t, size_t n) -> std::optional<std::vector<FastArrayField>> {
            size_t pos = 0;
            return read_fast_array_fields(cls, d, n, pos, {});
        });
        // (a commission pays: its reward lists items and experience maps the codec does not model, so this block is refused as a whole; its item's structure is checked around the reward)
        std::printf("  [info] a commission's block (7,670 bits): %s\n", fields ? "reads to its last bit" : "is refused (its reward has a map or an id list)");
        const ItemProbe commission = probe_cache_item(r->payload);
        CHECK(commission.found, "a commission's cache item is found by its headers alone");
        // the item's front and back are where the layout puts them: the instigator item's type id, then (at the end) the instance guid and a record id of the game's design data
        CHECK(commission.found && commission.type_id == 2616523457551378613ULL, "its front: the instigator item's type id is the item table's, as in an accept's");
        CHECK(commission.found && commission.guid_or != 0 && (commission.record >> 48) == 0x5429u, "its back: a guid and a record id of the design data, last");
        std::printf("  [info] the commission's item is %zu bits and ends with the record %llu\n", commission.item_bits, static_cast<unsigned long long>(commission.record));
        if (const Recorded* a = get("accept_component")) {
            const ItemProbe accept = probe_cache_item(a->payload);
            CHECK(accept.found && accept.item_bits == 2236 - 161 && accept.type_id == commission.type_id, "the same probe on the accept (read to its last bit): the 2,236-bit field less its 161-bit header, the same front");
            CHECK(accept.found && (accept.record >> 48) == (commission.record >> 48), "... and the same kind of record id at the back");
        }
    }
}

}  // namespace

int main() {
    test_codecs();
    test_schema();
    test_recorded();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
