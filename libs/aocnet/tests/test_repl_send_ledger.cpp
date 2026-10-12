// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  libs/aocnet/tests/test_repl_send_ledger.cpp
//
//  plan E1, stage 1, fix 2: UE's NAK handling for replicated properties (protocol/repl/send_ledger.h): a lost update's properties and fast-array
//  items go out again from their CURRENT values; what was acknowledged, or carried by a newer chain, does not.
//
//  A server actor (an NPC's class, a UNPCInfo and a UAoCStatsComponent with a fast array of stat items), its ActorShadow and a SendLedger; a
//  client that applies what each chain carries (read back with the replicator's own readers: the RepLayout block, then the fast-array fields);
//  a link that loses chains and reports each chain's fate (an AckEvent) some steps later, in the order the Connection reports them.  The checks
//  say what the client holds against what the server holds.
//
//    one lost update          a handle changed once and its packet lost: the next update (no other change) carries the current value
//    first, last              the first update after the open lost; the last lost; the quiet that follows still delivers it
//    newest wins              the value changed again after the lost packet: the NAK sends the newest, once; a newer chain that was
//                             acknowledged is not sent again for an older loss; the NAK of an older chain does not undo a newer value
//    acked is final           an acknowledged chain's record is dropped; nothing is sent for it, ever
//    bursts                   a run of lost chains, each with its own handles and items, is put right by one update
//    fast arrays              adds, changes and removes under loss, in every order the loss can take, with an item added then removed while
//                             its add was unacknowledged; the array's key equals the shadow's and it is still sent
//    a new subobject          the first block of a subobject lost: the whole block (and its export) goes out again
//    reliable chains          a lost reliable part is not a NAK (the Connection sends it again); the chain ends when every part was delivered;
//                             a reliable chain carrying a newer value takes the entry from an older unreliable one
//    parts                    one part of an unreliable chain lost is a NAK of the chain; all parts delivered end it
//    bounds                   records and entries end with their events; the table never holds more than kMaxRecords (an eviction is a NAK);
//                             clear() (a closed channel) drops everything and a late event finds nothing
//    soak                     seeded random operations on every property and array, random loss in random bursts, events delayed at random:
//                             the client always catches up, and the ledger is empty at the end
// ============================================================================
#include "protocol/repl/fast_array.h"
#include "protocol/repl/rep_block.h"
#include "protocol/repl/replicator.h"
#include "protocol/repl/send_ledger.h"
#include "protocol/repl/subobject.h"
#include "protocol/schema/generated/index.h"
// C-Y-B + CoA

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
using emit::BunchWriter;
using emit::FIntrepidNetworkGUID;

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

static FIntrepidNetworkGUID guid(uint64_t id) {
    FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = 60;
    g.Randomizer = 0x6ee87ee4;
    return g;
}

static void set_value(ObjectState& o, const char* path, Value v) {
    if (!o.state.set(*o.schema, path, std::move(v))) {
        std::printf("  [FAIL] %s.%s refused\n", o.schema->name, path);
        g_fail++;
    }
}

constexpr uint64_t kActorId = 2000, kInfoId = 2006, kStatsId = 2016, kLateId = 2020;

static Subobject make_sub(uint64_t id, const char* name, uint32_t checksum, const char* cls) {
    Subobject s;
    s.guid = guid(id);
    s.name = name;
    s.checksum = checksum;
    s.obj.schema = &schema_named(cls);
    return s;
}

static ElementState stat_item(int64_t record, uint32_t value) {
    ElementState es;
    es.values[1] = Value(StatInt32Inst{record, value, false, 0, 0});
    return es;
}

// ---- the client ----------------------------------------------------------------------------------------

struct ClientObject {
    std::map<uint16_t, Value>        values;
    std::map<uint32_t, std::map<int32_t, ElementState>> items;   // fast-array field -> ReplicationID -> item
    bool                             known = false;
};

struct Client {
    std::map<uint64_t, ClientObject> objects;   // 0 the actor, else the subobject's ObjectId
    size_t chains = 0, exports_seen = 0;

    void seed(const Actor& a) {
        read_object(0, a.obj);
        for (const Subobject& s : a.subobjects) read_object(s.guid.ObjectId, s.obj);
    }
    void read_object(uint64_t id, const ObjectState& o) {
        ClientObject& c = objects[id];
        c.known = true;
        c.values = o.state.values();
        for (const auto& [field, st] : o.arrays.all()) {
            for (const FastArrayItem& it : st.items()) c.items[field][it.id] = it.values;
        }
    }

    /// Applies the content blocks of one chain.
    bool apply(const Actor& a, const Emission& e) {
        ++chains;
        exports_seen += e.exports.size();
        const auto blocks = read_content_blocks(e.payload.data(), e.payload.bit_pos(), 0);
        if (!blocks) return false;
        for (const BlockView& b : *blocks) {
            const ObjectState* o = &a.obj;
            uint64_t id = 0;
            if (!b.header.is_actor) {
                id = b.header.object.ObjectId;
                const Subobject* s = a.find(b.header.object);
                if (!s) return false;
                o = &s->obj;
            }
            ClientObject& c = objects[id];
            c.known = true;
            const size_t end = b.payload_pos + b.payload_bits;
            size_t pos = b.payload_pos;
            if (b.header.has_rep_layout) {
                const auto st = read_rep_block(*o->schema, e.payload.data(), end, pos, o->opts);
                if (!st) return false;
                for (const auto& [h, v] : st->values()) c.values[h] = v;
            }
            if (pos < end) {
                const auto fields = read_fast_array_fields(*o->schema, e.payload.data(), end, pos, o->opts);
                if (!fields) return false;
                for (const FastArrayField& f : *fields) {
                    auto& items = c.items[f.field];
                    for (const int32_t id2 : f.delta.deleted) items.erase(id2);
                    for (const auto& ch : f.delta.changed) items[ch.id] = ch.values;
                }
            }
        }
        return true;
    }
};

// ---- the server and its link ---------------------------------------------------------------------------

struct Sent {
    uint64_t chain = 0;
    bool     reliable = false;
    size_t   parts = 1;
    bool     lost = false;
};
// built by C-O-A with C_Y_B

struct Rig {
    Actor        actor;
    ActorShadow  shadow;
    SendLedger   ledger;
    ExportAcks   acks;
    Client       client;
    uint64_t     next_chain = 1;
    std::deque<Sent> pending;       // chains whose event has not been reported yet, in order
    size_t       updates = 0, empty_updates = 0;
    size_t       exports_with_chains = 0;
    bool         bad = false;       // a chain that did not build or apply

    Rig() {
        actor.guid = guid(kActorId);
        actor.obj.schema = &schema_named("ANPCCharacter");
        set_value(actor.obj, "AuthServerIDReplicated", Value::u64(60));
        set_value(actor.obj, "RemoteRole", Value::u64(1));
        set_value(actor.obj, "ReplicatedMovementMode", Value::u64(10));
        Subobject info = make_sub(kInfoId, "BaseCharacterInfo", 0xaff9746f, "UNPCInfo");
        set_value(info.obj, "Level", Value::i64(14));
        set_value(info.obj, "CreatureOwnerName", Value::str("A Wolf"));
        actor.subobjects.push_back(std::move(info));
        Subobject stats = make_sub(kStatsId, "StatsComponent", 0x1234abcd, "UAoCStatsComponent");
        FastArrayState* owner = stats.obj.arrays.get(*stats.obj.schema, "StatRepInt32Owner");
        if (!owner) { std::printf("  [FAIL] no StatRepInt32Owner\n"); std::exit(2); }
        owner->add(stat_item(7001, 100));
        owner->add(stat_item(7002, 200));
        actor.subobjects.push_back(std::move(stats));
        // the open: the shadow becomes what an open leaves, and the client what an open tells it
        std::vector<ContentBlock> blocks;
        if (!collect_open_blocks(actor, shadow, blocks)) { std::printf("  [FAIL] the open does not collect\n"); std::exit(2); }
        client.seed(actor);
    }

    ObjectState& info() { return actor.subobjects[0].obj; }
    ObjectState& stats() { return actor.subobjects[1].obj; }
    FastArrayState& owner() { return *stats().arrays.get(*stats().schema, "StatRepInt32Owner"); }

    void set_level(int v) { set_value(info(), "Level", Value::i64(v)); }
    void set_name(const std::string& v) { set_value(info(), "CreatureOwnerName", Value::str(v)); }
    void set_mode(int v) { set_value(actor.obj, "ReplicatedMovementMode", Value::u64(static_cast<uint64_t>(v))); }
    void set_role(int v) { set_value(actor.obj, "RemoteRole", Value::u64(static_cast<uint64_t>(v))); }

    /// One update, as ActorView does it: built on a copy of the shadow, committed when the chain is accepted.  `lost`: the link will lose it.
    /// False when nothing changed.
    bool update(bool lost, bool reliable = false, size_t parts = 1) {
        ActorShadow work = shadow;
        bool failed = false;
        SentRecord record;
        const auto em = update_actor(actor, work, acks, &failed, &record);
        if (failed) { bad = true; return false; }
        if (!em) { ++empty_updates; return false; }
        ++updates;
        shadow = std::move(work);
        const uint64_t chain = next_chain++;
        ledger.sent(chain, reliable, parts, record, shadow);
        if (!em->exports.empty()) ++exports_with_chains;
        if (!lost && !client.apply(actor, *em)) bad = true;
        pending.push_back(Sent{chain, reliable, parts, lost});
        return true;
    }

    /// The connection reports the oldest `n` chains' fates (a chain of `parts` parts: one event each; a lost one reports its first part lost).
    void events(size_t n = SIZE_MAX) {
        while (n-- > 0 && !pending.empty()) {
            const Sent s = pending.front();
            pending.pop_front();
            if (s.lost && s.reliable) {
                ledger.on_event(s.chain, false, shadow);   // a reliable part: the Connection sends it again, and the event follows
                for (size_t i = 0; i < s.parts; ++i) ledger.on_event(s.chain, true, shadow);
            } else if (s.lost) {
                ledger.on_event(s.chain, false, shadow);
                for (size_t i = 1; i < s.parts; ++i) ledger.on_event(s.chain, true, shadow);   // the other parts came
            } else {
                for (size_t i = 0; i < s.parts; ++i) ledger.on_event(s.chain, true, shadow);
            }
        }
    }

    /// Whether the client holds what the server holds.
    bool converged(std::string* why = nullptr) const {
        auto fail = [&](const std::string& what) {
            if (why) *why = what;
            return false;
        };
        auto same = [&](uint64_t id, const ObjectState& o, const char* name) {
            const auto it = client.objects.find(id);
            if (it == client.objects.end() || !it->second.known) return fail(std::string(name) + " unknown to the client");
            const ClientObject& c = it->second;
            for (const auto& [h, v] : o.state.values()) {
                const auto cv = c.values.find(h);
                if (cv == c.values.end() || !(cv->second == v)) return fail(std::string(name) + " handle " + std::to_string(h));
            }
            for (const auto& [field, st] : o.arrays.all()) {
                const auto ci = c.items.find(field);
                const size_t have = ci == c.items.end() ? 0 : ci->second.size();
                if (have != st.items().size()) return fail(std::string(name) + " field " + std::to_string(field) + " has " + std::to_string(have) + " items, the server " + std::to_string(st.items().size()));
                for (const FastArrayItem& it2 : st.items()) {
                    const auto cv = ci->second.find(it2.id);
                    if (cv == ci->second.end() || !(cv->second == it2.values)) return fail(std::string(name) + " item " + std::to_string(it2.id));
                }
            }
            return true;
        };
        if (!same(0, actor.obj, "actor")) return false;
        for (const Subobject& s : actor.subobjects) {
            if (!same(s.guid.ObjectId, s.obj, s.name.c_str())) return false;
        }
        return true;
    }

    /// Lossless quiet: events reported, updates until nothing is sent (the NAKs' resends), at most `limit` rounds.
    bool settle(int limit = 20) {
        for (int i = 0; i < limit; ++i) {
            events();
            if (!update(false)) {
                events();
                return pending.empty();
            }
        }
        return false;
    }
};

// ---- tests ------------------------------------------------------------------------------------------------

static void test_one_lost_update() {
    std::printf("one lost update\n");
    Rig r;
    r.set_level(15);
    CHECK(r.update(/*lost=*/true), "a changed handle goes out");
    CHECK(!r.converged(), "(the client missed it: it still has level 14)");
    CHECK(!r.update(false), "before the NAK arrives the shadow says it was sent: nothing more goes out");
    r.events();
    CHECK(r.ledger.resent() == 1, "the NAK marks the one handle");
    CHECK(r.update(false), "the next update carries it again (nothing else changed)");
    r.events();
    CHECK(r.converged() && r.ledger.records() == 0 && r.ledger.entries() == 0, "the client holds level 15; the ledger is empty");
    CHECK(!r.update(false) && r.empty_updates >= 2, "and nothing is sent after that");

    // an actor-level handle and a subobject's string, lost together: one update puts both right
    Rig q;
    q.set_mode(11);
    q.set_name("A Dire Wolf");
    q.update(true);
    q.events();
    CHECK(q.update(false), "two handles of two objects, lost in one chain: one update carries both");
    q.events();
    CHECK(q.converged() && q.bad == false, "the client has both");
}

static void test_first_and_last() {
    std::printf("the first and the last packet\n");
    Rig r;
    // the first update after the open is lost, the next two arrive
    r.set_level(15);
    r.update(true);
    r.set_name("B");
    r.update(false);
    r.set_mode(12);
    r.update(false);
    r.events();
    CHECK(r.settle() && r.converged(), "the first of three updates lost: the client catches up with its value only");
    CHECK(r.client.chains == 2 + 1, "(two chains arrived, one more carried the lost handle)");
    // the last one lost, then quiet
    Rig s;
    s.set_level(16);
    s.update(false);
    s.set_name("C");
    s.update(false);
    s.set_mode(13);
    s.update(true);
    s.events();
    CHECK(s.settle() && s.converged(), "the last update lost, then silence: the NAK alone makes the server send it");
    // every one lost
    Rig t;
    for (int i = 0; i < 6; ++i) {
        t.set_level(20 + i);
        t.set_mode(20 + i);
        t.update(true);
    }
    t.events();
    CHECK(t.settle() && t.converged(), "six lost in a row, each changing the same two handles: the newest values arrive");
    CHECK(t.client.chains == 1, "...in one chain, not six");
}
// C-O-A / C Y B

static void test_newest_wins() {
    std::printf("the newest value wins\n");
    // changed again after the lost packet, the later packet arrives: nothing is sent for the old loss
    Rig r;
    r.set_level(15);
    r.update(true);
    r.set_level(16);
    r.update(false);
    r.events();
    CHECK(r.ledger.resent() == 0 && !r.update(false), "level 15 lost, level 16 delivered: the NAK of the first sends nothing (a newer chain carried it)");
    CHECK(r.converged(), "the client has 16");

    // both lost: the NAKs send 16 once
    Rig q;
    q.set_level(15);
    q.update(true);
    q.set_level(16);
    q.update(true);
    q.events();
    CHECK(q.update(false), "both lost: one update goes out");
    q.events();
    const Value* v = &q.client.objects[kInfoId].values.at(q.info().schema->command("Level")->handle);
    CHECK(*v == Value::i64(16) && q.converged(), "...with the newest value, 16 (never the old 15)");
    CHECK(q.client.chains == 1, "(the client received that one chain only)");

    // lost, then changed again but not sent yet when the NAK arrives: the one update carries the newest
    Rig p;
    p.set_level(15);
    p.update(true);
    p.events();
    p.set_level(17);
    CHECK(p.update(false), "the NAK, then a change before the next update");
    p.events();
    CHECK(p.converged() && p.client.chains == 1, "one chain, level 17");

    // the older chain is acknowledged late, after a newer one was lost: the newer one's NAK sends the value
    Rig o;
    o.set_level(15);
    o.update(false);
    o.set_level(16);
    o.update(true);
    o.events();
    CHECK(o.settle() && o.converged(), "15 delivered, 16 lost: 16 arrives");

    // a NAK of an older chain does not undo a newer acknowledged value, even when its event is reported late
    Rig n;
    n.set_mode(11);
    n.update(true);   // chain 1: lost
    n.set_mode(12);
    n.update(false);  // chain 2: delivered
    n.events(1);      // chain 1's NAK first
    CHECK(!n.update(false), "the NAK of chain 1 (superseded by chain 2): no update");
    n.events();
    CHECK(n.converged() && n.ledger.records() == 0, "the client has 12 and the ledger is empty");
}

static void test_acked_is_final() {
    std::printf("an acknowledged chain is final\n");
    Rig r;
    r.set_level(15);
    r.set_mode(11);
    r.update(false);
    CHECK(r.ledger.records() == 1 && r.ledger.entries() == 2, "an update in flight: one record, two entries");
    r.events();
    CHECK(r.ledger.records() == 0 && r.ledger.entries() == 0 && r.ledger.resent() == 0, "acknowledged: the record and its entries are gone");
    CHECK(!r.ledger.on_event(1, false, r.shadow), "a late NAK for the same chain finds nothing");
    CHECK(!r.update(false), "nothing is sent");
    // the other entries of a partly lost history stay untouched
    Rig q;
    q.set_level(15);
    q.update(false);
    q.set_mode(11);
    q.update(true);
    q.events();
    CHECK(q.update(false), "only the lost chain's handle goes again");
    CHECK(q.client.chains == 2 && q.converged(), "(two chains reached the client: the first, then the one that put the lost handle right)");
}
// built by C O A with C-Y-B

static void test_bursts() {
    std::printf("loss bursts\n");
    Rig r;
    for (int i = 0; i < 8; ++i) {
        switch (i % 4) {
            case 0: r.set_level(30 + i); break;
            case 1: r.set_name("n" + std::to_string(i)); break;
            case 2: r.set_mode(30 + i); break;
            case 3: r.owner().add(stat_item(8000 + i, static_cast<uint32_t>(i))); break;
        }
        r.update(/*lost=*/true);
    }
    r.events();
    CHECK(r.ledger.records() == 0, "eight lost chains reported: no record left");
    CHECK(r.update(false), "one update");
    r.events();
    CHECK(r.converged(), "carries all four kinds of change, each once");
    CHECK(r.client.chains == 1 && r.ledger.records() == 0, "(one chain reached the client)");

    // the burst in the middle of delivered ones
    Rig q;
    for (int i = 0; i < 12; ++i) {
        q.set_level(40 + i);
        q.set_role(i % 3);
        q.update(i >= 4 && i < 9);
    }
    q.events();
    CHECK(q.settle() && q.converged(), "a burst of five lost between delivered chains: caught up");
}

static void test_fast_arrays() {
    std::printf("fast arrays under loss\n");
    // an add, lost
    Rig a;
    a.owner().add(stat_item(7100, 1));
    a.update(true);
    a.events();
    CHECK(a.settle() && a.converged() && a.client.objects[kStatsId].items.begin()->second.size() == 3, "an item added in a lost chain arrives with the NAK");

    // a change, lost; the array key equals the shadow's, the item is sent all the same
    Rig c;
    const int32_t id1 = c.owner().items().front().id;
    c.owner().change(id1, stat_item(7001, 555));
    c.update(true);
    c.events();
    CHECK(c.update(false), "a changed item, lost: sent again although the array's key is the shadow's");
    c.events();
    CHECK(c.converged(), "the client has the new value");

    // a remove, lost
    Rig d;
    d.owner().remove(d.owner().items().front().id);
    d.update(true);
    d.events();
    CHECK(!d.converged(), "(the client still has the removed item)");
    CHECK(d.update(false), "a removal lost: the deletion goes out again");
    d.events();
    CHECK(d.converged() && d.client.objects[kStatsId].items.begin()->second.size() == 1, "the client dropped the item");

    // add lost, then removed before the NAK: nothing the client needs, nothing wrong
    Rig e;
    const auto added = e.owner().add(stat_item(7200, 9));
    e.update(true);
    e.owner().remove(*added);
    e.update(false);
    e.events();
    CHECK(e.settle() && e.converged(), "an item added in a lost chain and removed in a delivered one: the client never had it");

    // change lost, then the item removed in a delivered chain: a NAK must not bring it back
    Rig f;
    const int32_t id2 = f.owner().items().front().id;
    f.owner().change(id2, stat_item(7001, 321));
    f.update(true);
    f.owner().remove(id2);
    f.update(false);
    f.events();
    CHECK(f.settle() && f.converged(), "an item changed in a lost chain and removed in a delivered one: gone on the client");

    // remove lost, then the same item cannot return: others added meanwhile
    Rig g;
    const int32_t id3 = g.owner().items().front().id;
    g.owner().remove(id3);
    g.update(true);
    g.owner().add(stat_item(7300, 1));
    g.update(false);
    g.events();
    CHECK(g.settle() && g.converged(), "a removal lost, an add delivered: the client has neither the removed item nor misses the added");

    // everything at once, every chain lost
    Rig h;
    for (int i = 0; i < 6; ++i) {
        h.owner().add(stat_item(7400 + i, static_cast<uint32_t>(i)));
        h.update(true);
        if (i % 2) {
            h.owner().remove(h.owner().items().front().id);
            h.update(true);
        }
        h.owner().change(h.owner().items().back().id, stat_item(7499, static_cast<uint32_t>(i + 50)));
        h.update(true);
    }
    h.events();
    CHECK(h.settle() && h.converged(), "adds, changes and removes in lost chains, in a row: converged after one round");
}
// -- C-Y-B . C | O | A --

static void test_new_subobject() {
    std::printf("a subobject added after the open\n");
    Rig r;
    Subobject late = make_sub(kLateId, "CombatInfo", 0xb9f1a7bc, "UNPCCombatInfo");
    set_value(late.obj, "EliteRank", Value::i64(2));
    r.actor.subobjects.push_back(std::move(late));
    r.update(/*lost=*/true);
    CHECK(r.client.objects.count(kLateId) == 0 && r.exports_with_chains == 1, "its first block goes out with its export, and is lost");
    r.events();
    CHECK(r.update(false), "the NAK: the subobject is new again, its block and export are sent in full");
    r.events();
    CHECK(r.exports_with_chains == 2 && r.converged() && r.client.objects.count(kLateId) == 1, "the client has it now");
    // its later changes, lost with the first block's chain being fine
    Rig q;
    Subobject late2 = make_sub(kLateId, "CombatInfo", 0xb9f1a7bc, "UNPCCombatInfo");
    set_value(late2.obj, "EliteRank", Value::i64(1));
    q.actor.subobjects.push_back(std::move(late2));
    q.update(false);
    q.actor.subobjects.back().obj.state.set(*q.actor.subobjects.back().obj.schema, "EliteRank", Value::i64(3));
    q.update(true);
    q.events();
    CHECK(q.settle() && q.converged(), "a subobject's handle changed after its first block arrived, lost: caught up");
}

static void test_reliable_chains() {
    std::printf("reliable chains\n");
    Rig r;
    r.set_level(15);
    r.update(/*lost=*/true, /*reliable=*/true, /*parts=*/2);
    r.events();   // the lost part is reported, and the Connection's resend delivers it
    CHECK(r.ledger.resent() == 0 && r.ledger.records() == 0, "a lost part of a reliable chain is no NAK; the chain ends when its parts were delivered");
    CHECK(!r.update(false), "nothing is sent again by the ledger");

    // a reliable chain with a newer value takes the entry from an older unreliable one
    Rig q;
    q.set_level(15);
    q.update(true, /*reliable=*/false);
    q.set_level(16);
    q.update(false, /*reliable=*/true);
    q.events(1);
    CHECK(q.ledger.resent() == 0 && !q.update(false), "an unreliable 15 lost, a reliable 16 after it: the NAK sends nothing");
    q.events();
    CHECK(q.converged() && q.ledger.records() == 0, "converged");

    // an unreliable chain after a reliable one takes the entry from it: its loss is a NAK
    Rig p;
    p.set_level(15);
    p.update(false, true);
    p.set_level(16);
    p.update(true, false);
    p.events();
    CHECK(p.settle() && p.converged(), "reliable 15, then unreliable 16 lost: 16 arrives");
}

static void test_parts() {
    std::printf("the parts of a chain\n");
    Rig r;
    r.set_level(15);
    r.update(false, false, 3);
    CHECK(r.ledger.records() == 1, "an unreliable chain of three parts in flight");
    CHECK(r.ledger.on_event(1, true, r.shadow) && r.ledger.on_event(1, true, r.shadow) && r.ledger.records() == 1, "two parts delivered: still in flight");
    CHECK(r.ledger.on_event(1, true, r.shadow) && r.ledger.records() == 0 && r.ledger.resent() == 0, "the third delivered: the chain ended, nothing resent");
    Rig q;
    q.set_level(15);
    q.update(true, false, 3);   // the first part lost, the others delivered
    q.events();
    CHECK(q.ledger.resent() == 1 && q.ledger.records() == 0, "one part lost: a NAK of the whole chain (the others are of no use alone), once");
    CHECK(q.settle() && q.converged(), "the value arrives");
}
// ~ CoA & C:Y:B ~

static void test_bounds() {
    std::printf("bounds\n");
    Rig r;
    // more chains in flight than the table holds: the oldest are given up on, as NAKs
    const size_t n = SendLedger::kMaxRecords + 50;
    size_t peak_records = 0, peak_entries = 0;
    for (size_t i = 0; i < n; ++i) {
        r.set_level(100 + static_cast<int>(i));
        r.set_mode(static_cast<int>(i % 50));
        r.update(/*lost=*/true);
        peak_records = std::max(peak_records, r.ledger.records());
        peak_entries = std::max(peak_entries, r.ledger.entries());
    }
    CHECK(peak_records <= SendLedger::kMaxRecords && peak_entries <= 2 * SendLedger::kMaxRecords, "records and entries stay bounded while events never come");
    r.events();
    CHECK(r.settle() && r.converged() && r.ledger.records() == 0 && r.ledger.entries() == 0, "the late events end them all; the client is right");
    // an eviction is a NAK: a value only the evicted chain carried is not forgotten
    Rig q;
    q.set_role(3);
    q.update(true);   // chain 1: RemoteRole, the only one to carry it
    for (size_t i = 0; i < SendLedger::kMaxRecords; ++i) {   // chain 1 and these: one over the table, so chain 1 is evicted by the last
        q.set_level(200 + static_cast<int>(i));
        q.update(true);
    }
    CHECK(!q.ledger.tracks(1), "(chain 1 was evicted)");
    CHECK(q.update(false), "its handle was put back by the eviction: an update carries it");
    const Value* role = &q.client.objects[0].values.at(q.actor.obj.schema->command("RemoteRole")->handle);
    CHECK(*role == Value::u64(3), "the client has the RemoteRole only the evicted chain carried");
    q.events();
    CHECK(q.settle() && q.converged(), "and the rest arrives with the late events");

    // clear(): a closed channel
    Rig c;
    c.set_level(15);
    c.update(true);
    c.set_mode(11);
    c.update(true);
    c.ledger.clear();
    CHECK(c.ledger.records() == 0 && c.ledger.entries() == 0, "clear() drops every record");
    c.events();
    CHECK(c.ledger.resent() == 0, "a late NAK after the channel closed finds nothing");
}

// ---- the soak ----------------------------------------------------------------------------------------------

struct Soak {
    unsigned seed;
    unsigned loss_percent;
    unsigned burst;
    bool     mix_reliable;
};

static bool soak_run(const Soak& cfg, size_t steps, std::string* why) {
    std::mt19937 rng(cfg.seed);
    Rig r;
    unsigned burst_left = 0;
    size_t max_records = 0;
    for (size_t step = 0; step < steps; ++step) {
        // some random operations on random objects
        const unsigned ops = rng() % 4;
        for (unsigned k = 0; k < ops; ++k) {
            switch (rng() % 9) {
                case 0: r.set_level(static_cast<int>(rng() % 1000)); break;
                case 1: r.set_name("n" + std::to_string(rng() % 100)); break;
                case 2: r.set_mode(static_cast<int>(rng() % 20)); break;
                case 3: r.set_role(static_cast<int>(rng() % 4)); break;
                case 4: r.owner().add(stat_item(9000 + rng() % 50, rng() % 1000)); break;
                case 5:
                case 6:
                    if (!r.owner().items().empty()) {
                        const int32_t id = r.owner().items()[rng() % r.owner().items().size()].id;
                        r.owner().change(id, stat_item(9000 + rng() % 50, rng() % 1000));
                    }
                    break;
                case 7:
                    if (r.owner().items().size() > 1) r.owner().remove(r.owner().items()[rng() % r.owner().items().size()].id);
                    break;
                case 8: r.owner().mark_array_dirty(); break;
            }
        }
        // a chain: lost in bursts at the configured rate; with a few reliable ones (never lost: the Connection resends those)
        bool lost = false;
        if (burst_left > 0) {
            lost = true;
            --burst_left;
        } else if (rng() % 100 < cfg.loss_percent) {
            lost = true;
            burst_left = cfg.burst > 0 ? rng() % cfg.burst : 0;
        }
        const bool reliable = cfg.mix_reliable && rng() % 7 == 0;
        r.update(lost && !reliable, reliable, 1 + rng() % 2);
        if (r.bad) {
            if (why) *why = "a chain did not build or apply";
            return false;
        }
        // the connection reports some chains' fates, a few steps late
        const size_t report = rng() % 4;
        if (r.pending.size() > report + 3) r.events(r.pending.size() - report - 3);
        max_records = std::max(max_records, r.ledger.records());
    }
    if (max_records > SendLedger::kMaxRecords) {
        if (why) *why = "the ledger outgrew its bound";
        return false;
    }
    if (!r.settle(40)) {
        if (why) *why = "the lossless tail never went quiet";
        return false;
    }
    std::string reason;
    if (!r.converged(&reason)) {
        if (why) *why = "the client did not catch up: " + reason;
        return false;
    }
    if (r.ledger.records() != 0 || r.ledger.entries() != 0) {
        if (why) *why = "the ledger is not empty at the end";
        return false;
    }
    return true;
}

static void test_soak() {
    std::printf("a seeded soak: random operations, random loss in bursts, late events\n");
    const Soak configs[] = {{1, 5, 1, false}, {2, 20, 3, false}, {3, 40, 8, false}, {4, 70, 12, false}, {5, 20, 4, true}, {6, 50, 10, true}, {7, 0, 1, false}};
    size_t failures = 0;
    for (const Soak& c : configs) {
        for (unsigned s = 0; s < 40; ++s) {
            Soak cc = c;
            cc.seed = c.seed * 1000 + s;
            std::string why;
            if (!soak_run(cc, 600, &why)) {
                ++failures;
                std::printf("    seed %u (loss %u%%, burst %u, reliable %d): %s\n", cc.seed, cc.loss_percent, cc.burst, cc.mix_reliable ? 1 : 0, why.c_str());
            }
        }
    }
    CHECK(failures == 0, "280 runs of 600 steps: the client always catches up, the ledger ends empty and bounded");
}

int main() {
    test_one_lost_update();
    test_first_and_last();
    test_newest_wins();
    test_acked_is_final();
    test_bursts();
    test_fast_arrays();
    test_new_subobject();
    test_reliable_chains();
    test_parts();
    test_bounds();
    test_soak();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
