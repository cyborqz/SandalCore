// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_actor_nak.cpp  (plan E1, stage 1, fix 2)
//
//  Lost packets and the world's actors: the actor system on a real Connection with a fake client that drops chosen packets (and chosen
//  acknowledgements) and reads back what reaches it.  The view enters every update chain in a SendLedger (libaocnet protocol/repl/send_ledger.h,
//  tested on its own in test_repl_send_ledger) and the Connection's AckEvents decide: a chain with a lost part puts its handles and fast-array
//  items back, so the next update sends their CURRENT values.
//
//    movement           ReplicatedMovement changes sent unreliably (Change::Movement); the packet of one lost: the client's value catches up with
//                       no further change; the first and the last of a run lost; a run of lost ones; changed again after the lost packet
//                       (the newest is sent, once); a NAK of a chain a later one superseded sends nothing; every update on its own chain
//    stat items         the fast array of a UAoCStatsComponent: adds, changes and removes in unreliable chains under loss, the key equals the
//                       shadow's and the item is still sent
//    reliable           a state change (reliable): the Connection sends the lost bunch again; the ledger does not (no NAK)
//    acknowledgements   lost acknowledgements of delivered packets: the client's packet is dropped, the NAK for what it did get is a late
//                       resend of current values, never a wrong value
//    dormancy           an update lost just before the Dormancy close: the snapshot is stale, the actor is opened again with its current state
//    bounds             the ledger's records and the view's routes end with the events (0 at the end), stay under the bound while the client
//                       answers nothing, and a closed channel (Relevancy) forgets its chains
//    soak               a crowd of NPCs, seeded random changes of every kind, loss and bursts both ways: the client always catches up
// ============================================================================
#include "bootstrap/constants.h"
#include "fake_client.h"
#include "net/connection.h"
#include "protocol/repl/fast_array.h"
#include "protocol/repl/rep_block.h"
#include "protocol/repl/replicator.h"
#include "protocol/repl/subobject.h"
#include "protocol/schema/generated/index.h"
#include "session/actor_view.h"
#include "world/actor_registry.h"
#include "world/relevancy.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>
// ~ C_Y_B & CoA ~

using namespace aoc::worldd;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using emit::FIntrepidNetworkGUID;
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

static const schema::ClassSchema& schema_named(const char* name) {
    const schema::ClassSchema* s = schema::gen::find_class_schema(name);
    if (!s) { std::printf("  [FAIL] no schema %s\n", name); std::exit(2); }
    return *s;
}

static FIntrepidNetworkGUID guid(uint64_t id, uint32_t server = 60, uint32_t rnd = 0x6ee87ee4) {
    FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = server;
    g.Randomizer = rnd;
    return g;
}

static repl::ExportLink link(uint64_t id, const char* name, bool no_load, uint32_t checksum) {
    repl::ExportLink l;
    l.guid = guid(id, 0, 0);
    l.name = name;
    l.no_load = no_load;
    l.has_checksum = true;
    l.checksum = checksum;
    return l;
}

static void set_value(repl::ObjectState& o, const char* path, repl::Value v) {
    if (!o.state.set(*o.schema, path, std::move(v))) {
        std::printf("  [FAIL] %s.%s refused\n", o.schema->name, path);
        g_fail++;
    }
}

static repl::ElementState stat_item(int64_t record, uint32_t value) {
    repl::ElementState es;
    es.values[1] = repl::Value(repl::StatInt32Inst{record, value, false, 0, 0});
    return es;
}

static repl::RepMovement movement(double x, double y, int64_t vx = 0) {
    repl::RepMovement m;
    m.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, static_cast<int64_t>(x * 100), static_cast<int64_t>(y * 100), 1200000};
    m.rotation = repl::Rot{0, 64, 0};
    m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, vx, 0, 0};
    return m;
}

/// An NPC: the actor block, UNPCInfo, and a StatsComponent with two stat items.
static world::WorldActor make_npc(uint64_t id, double x, double y) {
    world::WorldActor w;
    w.id = id;
    w.kind = world::ActorKind::Npc;
    w.x = x;
    w.y = y;
    w.z = 12000;
    repl::Actor& a = w.actor;
    a.guid = guid(id);
    a.obj.schema = &schema_named("ANPCCharacter");
    set_value(a.obj, "AuthServerIDReplicated", repl::Value::u64(60));
    set_value(a.obj, "ReplicatedMovement", repl::Value(movement(x, y)));
    set_value(a.obj, "RemoteRole", repl::Value::u64(1));
    set_value(a.obj, "Owner", repl::Value(guid(id + 2)));
    set_value(a.obj, "Role", repl::Value::u64(4));
    set_value(a.obj, "Controller", repl::Value(guid(id + 2)));
    set_value(a.obj, "ReplicatedMovementMode", repl::Value::u64(10));
    set_value(a.obj, "CurrentMovementInput", repl::Value(repl::PlayerMovementInput{0}));
    a.open.archetype = {link(0xA001 + id % 7 * 2, "Default__Wolf_C", false, 0x1234), link(0xB001 + id % 7 * 2, "/Game/DesignAssets/Population/NPC/Beast/Wolf/Wolf", false, 0)};
    a.open.level = bootstrap::persistent_level_export();
    a.open.spawn.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, static_cast<int64_t>(x * 10), static_cast<int64_t>(y * 10), 120000};
    a.open.spawn.rotation = repl::Rot{0, 11000, 0};
    repl::Subobject info;
    info.guid = guid(id + 6);
    info.name = "BaseCharacterInfo";
    info.checksum = 0xaff9746f;
    info.obj.schema = &schema_named("UNPCInfo");
    set_value(info.obj, "CharacterGuid.A", repl::Value::i64(static_cast<int32_t>(id)));
    set_value(info.obj, "CreatureOwnerName", repl::Value::str("A Wolf"));
    set_value(info.obj, "Level", repl::Value::i64(14));
    a.subobjects.push_back(std::move(info));
    repl::Subobject stats;
    stats.guid = guid(id + 16);
    stats.name = "StatsComponent";
    stats.checksum = 0x1234abcd;
    stats.obj.schema = &schema_named("UAoCStatsComponent");
    repl::FastArrayState* owner = stats.obj.arrays.get(*stats.obj.schema, "StatRepInt32Owner");
    owner->add(stat_item(7001, 100));
    owner->add(stat_item(7002, 200));
    a.subobjects.push_back(std::move(stats));
    w.npc.name = "A Wolf";
    w.npc.level = 14;
    return w;
}

// ---- the client --------------------------------------------------------------------------------------------------------------
// -- C-Y-B . CoA --

struct ClientObject {
    std::map<uint16_t, repl::Value> values;
    std::map<uint32_t, std::map<int32_t, repl::ElementState>> items;
};

struct ActorModel {
    std::map<uint64_t, ClientObject> objects;   // 0: the actor, else the subobject's ObjectId
    bool opened = false;
    size_t opens = 0, updates = 0;
};

// ---- the rig ---------------------------------------------------------------------------------------------------------------------

struct Rig {
    world::ActorRegistry reg;
    Connection conn{ConnectionConfig{kOuter, kCf, 14265, 15870}, t0};
    fake::Client client{kOuter, kCf, 14265, 15870};
    std::unique_ptr<ActorView> view;
    Clock::time_point t = t0;
    double px = 0, py = 0;
    std::map<uint64_t, ActorModel> models;
    std::map<uint32_t, uint64_t> actor_on_channel;
    std::function<bool(size_t)> lose_to_client;   // by the index of the server's packet
    std::function<bool(size_t)> lose_to_server;   // by the index of the client's packet
    size_t server_packets = 0, client_packets = 0, lost_to_client = 0, lost_to_server = 0;
    size_t unreliable_chains = 0, reliable_chains = 0;
    bool unreadable = false;
    int step_no = 0;
    std::vector<std::string> log;   // the story of a run, printed when a soak fails
    void note(const std::string& s) { log.push_back(std::to_string(step_no) + ": " + s); }

    explicit Rig(ActorView::Config cfg = {}) {
        view = std::make_unique<ActorView>(reg, cfg, [](uint32_t i) { return i >= 3 && i <= 6; });
        view->set_trace([this](uint32_t, bool open, bool reliable, size_t) {
            if (open) return;
            ++(reliable ? reliable_chains : unreliable_chains);
        });
        view->set_chain_trace([this](ChainId chain, uint64_t actor, bool reliable, size_t parts, size_t entries) {
            note("chain " + std::to_string(chain) + " actor " + std::to_string(actor) + (reliable ? " reliable" : " unreliable") + " parts " + std::to_string(parts) + " entries now " + std::to_string(entries));
        });
    }

    uint64_t add(double x, double y) {
        static uint64_t next = 10000;
        const uint64_t id = next;
        next += 100;
        reg.add(make_npc(id, x, y));
        return id;
    }

    const world::WorldActor& actor(uint64_t id) const { return *reg.find(id); }

    /// Changes the actor the way a movement does: ReplicatedMovement only (an unreliable update).
    void move_to(uint64_t id, double x, double y, int64_t vx = 0) {
        reg.modify(id, world::Change::Movement, [&](world::WorldActor& a) {
            a.actor.obj.state.set(*a.actor.obj.schema, "ReplicatedMovement", repl::Value(movement(x, y, vx)));
        });
    }
    /// A change of the other kind: reliable.
    void set_mode(uint64_t id, int v) {
        reg.modify(id, world::Change::State, [&](world::WorldActor& a) {
            a.actor.obj.state.set(*a.actor.obj.schema, "ReplicatedMovementMode", repl::Value::u64(static_cast<uint64_t>(v)));
        });
    }
    /// Any change, counted as `kind` (the view chooses the chain's reliability from it).
    template <class F>
    void change(uint64_t id, world::Change kind, F&& f) {
        reg.modify(id, kind, [&](world::WorldActor& a) { f(a); });
    }
    static repl::FastArrayState& owner_of(world::WorldActor& a) {
        return *a.actor.subobjects[1].obj.arrays.get(*a.actor.subobjects[1].obj.schema, "StatRepInt32Owner");
    }
// ~ C | O | A & Cyb ~

    void seed(uint64_t id) {
        ActorModel& m = models[id];
        m.opened = true;
        ++m.opens;
        m.objects.clear();
        const world::WorldActor& a = actor(id);
        auto take = [&](uint64_t oid, const repl::ObjectState& o) {
            ClientObject& c = m.objects[oid];
            c.values = o.state.values();
            for (const auto& [field, st] : o.arrays.all()) {
                for (const repl::FastArrayItem& it : st.items()) c.items[field][it.id] = it.values;
            }
        };
        take(0, a.actor.obj);
        for (const repl::Subobject& s : a.actor.subobjects) take(s.guid.ObjectId, s.obj);
    }

    bool apply_update(uint64_t id, const fake::Delivered& d) {
        ActorModel& m = models[id];
        if (!m.opened) return true;   // an unreliable chain for a channel that is not open: the client drops it
        const repl::Actor& a = actor(id).actor;
        const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!blocks) return false;
        ++m.updates;
        for (const repl::BlockView& b : *blocks) {
            const repl::ObjectState* o = &a.obj;
            uint64_t oid = 0;
            if (!b.header.is_actor) {
                oid = b.header.object.ObjectId;
                const repl::Subobject* s = a.find(b.header.object);
                if (!s) return false;
                o = &s->obj;
            }
            ClientObject& c = m.objects[oid];
            const size_t end = b.payload_pos + b.payload_bits;
            size_t pos = b.payload_pos;
            if (b.header.has_rep_layout) {
                const auto st = repl::read_rep_block(*o->schema, d.content.data(), end, pos, o->opts);
                if (!st) return false;
                for (const auto& [h, v] : st->values()) c.values[h] = v;
            }
            if (pos < end) {
                const auto fields = repl::read_fast_array_fields(*o->schema, d.content.data(), end, pos, o->opts);
                if (!fields) return false;
                for (const repl::FastArrayField& f : *fields) {
                    auto& items = c.items[f.field];
                    for (const int32_t gone : f.delta.deleted) items.erase(gone);
                    for (const auto& ch : f.delta.changed) items[ch.id] = ch.values;
                }
            }
        }
        return true;
    }

    void absorb(const fake::Delivered& d) {
        if (d.channel == 0 || d.close) return;
        uint64_t id = 0;
        for (const auto& [ch, who] : actor_on_channel) {
            if (ch == d.channel) id = who;
        }
        if (d.open) {
            // the open's own channel is not in the map yet when the view opened it this very step: ask the view
            for (const auto& [aid, m] : models) (void)m;
            reg.for_each([&](const world::WorldActor& a) {
                if (view->channel_of(a.id) == d.channel) id = a.id;
            });
            if (id != 0) seed(id);
            return;
        }
        if (id == 0) return;
        if (!apply_update(id, d)) unreadable = true;
    }

    void step(milliseconds dt = milliseconds(67)) {
        ++step_no;
        t += dt;
        reg.for_each([&](const world::WorldActor& a) {
            const uint32_t ch = view->channel_of(a.id);
            if (ch != 0) actor_on_channel[ch] = a.id;
        });
        view->tick(conn, px, py, t);
        reg.for_each([&](const world::WorldActor& a) {
            const uint32_t ch = view->channel_of(a.id);
            if (ch != 0) actor_on_channel[ch] = a.id;
        });
        for (auto& pkt : conn.flush(t)) {
            ++server_packets;
            if (lose_to_client && lose_to_client(server_packets)) {
                ++lost_to_client;
                continue;
            }
            const auto r = client.receive(pkt);
            for (const auto& d : r.delivered) absorb(d);
        }
        const auto cp = client.packet({});
        ++client_packets;
        if (lose_to_server && lose_to_server(client_packets)) {
            ++lost_to_server;
        } else {
            conn.receive(cp, t);
        }
        for (const AckEvent& e : conn.take_ack_events()) {
            note("event chain " + std::to_string(e.chain) + " part " + std::to_string(e.part) + (e.delivered ? " delivered" : " LOST"));
            view->on_ack(e);
        }
    }
    void run(int steps, milliseconds dt = milliseconds(67)) {
        for (int i = 0; i < steps; ++i) step(dt);
    }

    bool converged(uint64_t id, std::string* why = nullptr) const {
        auto fail = [&](const std::string& what) {
            if (why) *why = what;
            return false;
        };
        const auto mi = models.find(id);
        if (mi == models.end() || !mi->second.opened) return fail("not open on the client");
        const ActorModel& m = mi->second;
        const world::WorldActor& a = actor(id);
        auto same = [&](uint64_t oid, const repl::ObjectState& o, const std::string& name) {
            const auto it = m.objects.find(oid);
            if (it == m.objects.end()) return fail(name + " unknown to the client");
            const ClientObject& c = it->second;
            for (const auto& [h, v] : o.state.values()) {
                const auto cv = c.values.find(h);
                if (cv == c.values.end() || !(cv->second == v)) return fail(name + " handle " + std::to_string(h));
            }
            for (const auto& [field, st] : o.arrays.all()) {
                const auto ci = c.items.find(field);
                const size_t have = ci == c.items.end() ? 0 : ci->second.size();
                if (have != st.items().size()) return fail(name + " field " + std::to_string(field) + ": " + std::to_string(have) + " items, the server " + std::to_string(st.items().size()));
                for (const repl::FastArrayItem& item : st.items()) {
                    const auto cv = ci->second.find(item.id);
                    if (cv == ci->second.end() || !(cv->second == item.values)) return fail(name + " item " + std::to_string(item.id));
                }
            }
            return true;
        };
        if (!same(0, a.actor.obj, "actor")) return false;
        for (const repl::Subobject& s : a.actor.subobjects) {
            if (!same(s.guid.ObjectId, s.obj, s.name)) return false;
        }
        return true;
    }
    bool all_converged(std::string* why = nullptr) const {
        bool ok = true;
        reg.for_each([&](const world::WorldActor& a) {
            std::string w;
            if (ok && !converged(a.id, &w)) {
                ok = false;
                if (why) *why = "actor " + std::to_string(a.id) + ": " + w;
            }
        });
        return ok;
    }
// [C-O-A - C | Y | B]

    /// The tail of a test: no loss, no changes; the view and the client settle.
    void quiet(int steps = 40) {
        lose_to_client = nullptr;
        lose_to_server = nullptr;
        run(steps);
    }
};

static bool in_window(size_t index, size_t first, size_t last) { return index >= first && index <= last; }

// ---- tests ------------------------------------------------------------------------------------------------------------------

static void test_movement_loss() {
    std::printf("a movement update lost\n");
    Rig r;
    const uint64_t id = r.add(1000, 0);
    r.run(6);   // the open and its acknowledgements
    CHECK(r.models[id].opened && r.converged(id), "(the client has the actor)");
    CHECK(r.view->ledger_records() == 0 && r.view->routes() == 0, "(nothing in flight)");

    // one update, its packet lost, nothing else changes afterwards
    const size_t before = r.server_packets;
    r.lose_to_client = [&](size_t i) { return i == before + 1; };
    r.move_to(id, 1010, 5);
    r.step();
    CHECK(r.unreliable_chains == 1 && r.reliable_chains == 0, "the movement goes out on an unreliable chain");
    CHECK(r.lost_to_client == 1 && !r.converged(id), "its packet is lost: the client does not have the new position");
    r.run(6);
    std::string why;
    CHECK(r.converged(id, &why), ("the NAK made the next update carry the current value: the client caught up (" + why + ")").c_str());
    CHECK(r.view->stats().naks == 1 && r.view->stats().nak_entries >= 1, "one NAK, put back what the chain carried");
    CHECK(r.unreliable_chains == 2 && r.models[id].updates == 1, "...in one more update chain, and nothing after it");
    const size_t updates_so_far = r.view->stats().updates_unreliable;
    r.run(10);
    CHECK(r.view->stats().updates_unreliable == updates_so_far && r.view->ledger_records() == 0 && r.view->routes() == 0, "then silence: nothing sent again, no record left");

    // the first of a run lost, the last lost
    Rig q;
    const uint64_t id2 = q.add(1000, 0);
    q.run(6);
    size_t base = q.server_packets;
    q.lose_to_client = [&](size_t i) { return i == base + 1; };   // the first update's packet
    for (int k = 0; k < 4; ++k) {
        q.move_to(id2, 1010 + 10 * k, 5 + k);
        q.step();
    }
    q.quiet(8);
    CHECK(q.all_converged(&why), ("the first of four updates lost: converged (" + why + ")").c_str());

    Rig z;
    const uint64_t id3 = z.add(1000, 0);
    z.run(6);
    base = z.server_packets;
    z.lose_to_client = [&](size_t i) { return i == base + 4; };   // the fourth update's packet (one packet a step)
    for (int k = 0; k < 4; ++k) {
        z.move_to(id3, 1010 + 10 * k, 5 + k);
        z.step();
    }
    z.quiet(8);
    CHECK(z.all_converged(&why), ("the last of four updates lost, nothing after it: converged (" + why + ")").c_str());

    // a burst: six in a row lost, each with a new position
    Rig b;
    const uint64_t id4 = b.add(1000, 0);
    b.run(6);
    base = b.server_packets;
    b.lose_to_client = [&](size_t i) { return in_window(i, base + 1, base + 6); };
    for (int k = 0; k < 6; ++k) {
        b.move_to(id4, 1100 + 10 * k, 7 * k);
        b.step();
    }
    b.quiet(10);
    CHECK(b.all_converged(&why), ("six lost in a row: converged (" + why + ")").c_str());
    CHECK(b.view->stats().naks >= 1 && b.models[id4].updates == 1, "...by one update carrying the newest position, not six");
}

static void test_newest_wins() {
    std::printf("the newest value is sent\n");
    // lost, then changed again before the NAK is reported: the one update after the NAK has the newest
    Rig r;
    const uint64_t id = r.add(1000, 0);
    r.run(6);
    r.lose_to_client = [&](size_t i) { return i == r.server_packets + 1; };
    r.move_to(id, 1010, 5);
    r.step();
    r.lose_to_client = nullptr;
    r.move_to(id, 1020, 9);   // changed again; its chain goes out next step, before or with the NAK
    r.run(8);
    std::string why;
    CHECK(r.converged(id, &why), ("converged on the newest position (" + why + ")").c_str());
    const repl::Value& v = r.models[id].objects[0].values.at(r.actor(id).actor.obj.schema->command("ReplicatedMovement")->handle);
    const repl::RepMovement* m = v.get_if<repl::RepMovement>();
    CHECK(m && m->location.x == 1020 * 100, "the client's position is the newest (10.20 km), not the lost one");

    // lost, and the next one (carrying the same handle) delivered: the NAK of the first sends nothing
    Rig q;
    const uint64_t id2 = q.add(1000, 0);
    q.run(6);
    const size_t base = q.server_packets;
    q.lose_to_client = [&](size_t i) { return i == base + 1; };
    q.move_to(id2, 1010, 5);
    q.step();
    q.move_to(id2, 1020, 9);
    q.step();
    q.quiet(8);
    CHECK(q.converged(id2, &why), ("lost 1010, delivered 1020: converged (" + why + ")").c_str());
    CHECK(q.view->stats().naks == 0 && q.unreliable_chains == 2, "the NAK found its handle carried by a newer chain: nothing was sent for it");
}

static void test_stat_items() {
    std::printf("fast-array items in lost chains\n");
    Rig r;
    const uint64_t id = r.add(1000, 0);
    r.run(6);
    std::string why;
    // add, change, remove: one chain each (unreliable), the chains of the adds and the removes lost
    size_t base = r.server_packets;
    r.lose_to_client = [&](size_t i) { return i == base + 1 || i == base + 3 || i == base + 5; };
    r.change(id, world::Change::Movement, [](world::WorldActor& a) { Rig::owner_of(a).add(stat_item(7100, 5)); });
    r.step();
    r.change(id, world::Change::Movement, [](world::WorldActor& a) { Rig::owner_of(a).change(Rig::owner_of(a).items().front().id, stat_item(7001, 111)); });
    r.step();
    r.change(id, world::Change::Movement, [](world::WorldActor& a) { Rig::owner_of(a).remove(Rig::owner_of(a).items()[1].id); });
    r.step();
    r.change(id, world::Change::Movement, [](world::WorldActor& a) { Rig::owner_of(a).add(stat_item(7200, 6)); });
    r.step();
    r.change(id, world::Change::Movement, [](world::WorldActor& a) { Rig::owner_of(a).change(Rig::owner_of(a).items().back().id, stat_item(7200, 66)); });
    r.step();
    r.quiet(12);
    CHECK(r.all_converged(&why), ("adds, changes and a remove in unreliable chains, half of them lost: the client's items are the server's (" + why + ")").c_str());
    CHECK(r.view->ledger_records() == 0, "(no record left)");

    // every chain lost
    Rig q;
    const uint64_t id2 = q.add(1000, 0);
    q.run(6);
    base = q.server_packets;
    q.lose_to_client = [&](size_t i) { return in_window(i, base + 1, base + 12); };
    for (int k = 0; k < 6; ++k) {
        q.change(id2, world::Change::Movement, [k](world::WorldActor& a) {
            Rig::owner_of(a).add(stat_item(7300 + k, static_cast<uint32_t>(k)));
            if (k % 2) Rig::owner_of(a).remove(Rig::owner_of(a).items().front().id);
        });
        q.step();
    }
    q.quiet(14);
    CHECK(q.all_converged(&why), ("six chains of adds and removes lost: converged (" + why + ")").c_str());
}
// (C.O.A : Cyb)

static void test_reliable_state() {
    std::printf("a reliable change lost\n");
    Rig r;
    const uint64_t id = r.add(1000, 0);
    r.run(6);
    const size_t base = r.server_packets;
    r.lose_to_client = [&](size_t i) { return i == base + 1; };
    r.set_mode(id, 11);
    r.step();
    CHECK(r.reliable_chains == 1 && r.lost_to_client == 1, "a state change goes out reliably, and its packet is lost");
    r.quiet(10);
    std::string why;
    CHECK(r.converged(id, &why), ("the Connection sent it again: converged (" + why + ")").c_str());
    CHECK(r.view->stats().naks == 0 && r.reliable_chains == 1, "the ledger sent nothing (no NAK for a reliable chain), the resend is the Connection's");
    CHECK(r.view->ledger_records() == 0 && r.view->routes() == 0, "(and the reliable chain's record ended with its acknowledgement)");

    // a reliable change after an unreliable one of the same handle: the older chain's loss does not undo it
    Rig q;
    const uint64_t id2 = q.add(1000, 0);
    q.run(6);
    const size_t b2 = q.server_packets;
    q.lose_to_client = [&](size_t i) { return i == b2 + 1; };
    q.change(id2, world::Change::Movement, [](world::WorldActor& a) { a.actor.obj.state.set(*a.actor.obj.schema, "ReplicatedMovementMode", repl::Value::u64(12)); });
    q.step();
    q.set_mode(id2, 13);   // reliable, same handle
    q.step();
    q.quiet(10);
    CHECK(q.converged(id2, &why), ("unreliable 12 lost, reliable 13 after it: the client has 13 (" + why + ")").c_str());
}

static void test_lost_acks() {
    std::printf("lost acknowledgements\n");
    Rig r;
    const uint64_t id = r.add(1000, 0);
    r.run(6);
    // the client's packets are lost for a while: every server packet arrives but the server hears nothing
    const size_t base = r.client_packets;
    r.lose_to_server = [&](size_t i) { return in_window(i, base + 1, base + 6); };
    for (int k = 0; k < 4; ++k) {
        r.move_to(id, 1010 + 10 * k, 3 * k);
        r.step();
    }
    r.run(4);
    std::string why;
    CHECK(r.converged(id, &why), ("(the updates themselves arrived: converged) " + why).c_str());
    r.quiet(10);
    CHECK(r.converged(id, &why), ("late acknowledgements, some of them reported as lost for packets the client had: still converged (" + why + ")").c_str());
    CHECK(r.view->ledger_records() == 0 && r.view->routes() == 0, "every chain's fate was reported in the end");
}

static void test_dormancy() {
    std::printf("an update lost before the Dormancy close\n");
    Rig r;
    const uint64_t id = r.add(0, 0);
    r.run(6);
    CHECK(r.view->state_of(id) == world::ClientState::Awake && r.converged(id), "(the actor is awake and the client has it)");
    // it changes, and the player walks off to 200 m in the same step: update, then the Dormancy close; the update's packet is lost
    r.move_to(id, 10, 20);
    r.px = 20000;
    const size_t base = r.server_packets;
    r.lose_to_client = [&](size_t i) { return i == base + 1; };
    r.step();
    r.lose_to_client = nullptr;
    CHECK(r.view->state_of(id) == world::ClientState::Dormant, "the actor went dormant");
    CHECK(!r.converged(id), "(the client's snapshot misses the last move)");
    r.run(10);
    std::string why;
    CHECK(r.view->stats().stale_snapshots == 1, "the NAK arrived: the snapshot is stale");
    CHECK(r.converged(id, &why), ("the actor was opened again as a snapshot with its current state (" + why + ")").c_str());
    CHECK(r.view->state_of(id) == world::ClientState::Dormant && r.view->ledger_records() == 0 && r.view->routes() == 0, "and closed again with Dormancy; nothing left in flight");

    // the same with the update delivered: no second snapshot
    Rig q;
    const uint64_t id2 = q.add(0, 0);
    q.run(6);
    q.move_to(id2, 10, 20);
    q.px = 20000;
    q.run(12);
    CHECK(q.view->stats().stale_snapshots == 0 && q.view->stats().snapshots == 0 && q.converged(id2), "a delivered update before the close: nothing more is sent");
}
// C O A / C-Y-B

static void test_bounds() {
    std::printf("bounds\n");
    // the client answers nothing for a long time: nothing grows without bound
    Rig r;
    const uint64_t id = r.add(1000, 0);
    r.run(6);
    r.lose_to_server = [](size_t) { return true; };
    size_t peak_records = 0, peak_routes = 0;
    for (int k = 0; k < 600; ++k) {
        r.move_to(id, 1000 + k, k % 50);
        r.step(milliseconds(5));
        peak_records = std::max(peak_records, r.view->ledger_records());
        peak_routes = std::max(peak_routes, r.view->routes());
    }
    CHECK(peak_records <= repl::SendLedger::kMaxRecords && peak_routes <= 4096, "600 updates while the server hears nothing: the records stay under the bound");
    r.quiet(150);
    std::string why;
    CHECK(r.converged(id, &why) && r.view->ledger_records() == 0 && r.view->routes() == 0, ("then the events come: converged, nothing left (" + why + ")").c_str());

    // a Relevancy close forgets the chains
    Rig q;
    const uint64_t id2 = q.add(0, 0);
    q.reg.for_each([](const world::WorldActor&) {});
    q.reg.modify(id2, world::Change::State, [](world::WorldActor& a) { a.never_dormant = true; });
    q.run(6);
    q.lose_to_server = [](size_t) { return true; };
    q.move_to(id2, 5, 5);
    q.step();
    CHECK(q.view->ledger_records() == 1 && q.view->routes() == 1, "an update in flight: one record, one route");
    q.px = 40000;   // far beyond the awake ring: the Relevancy close
    q.step();
    q.step();
    CHECK(q.view->state_of(id2) == world::ClientState::None && q.view->ledger_records() == 0 && q.view->routes() == 0, "the Relevancy close forgot the chain");
    q.quiet(10);
    CHECK(q.view->stats().naks == 0, "(its late event found nothing)");
}

// ---- the soak -----------------------------------------------------------------------------------------------------------------

static bool soak_run(unsigned seed, unsigned loss_percent, unsigned burst, size_t steps, std::string* why) {
    std::mt19937 rng(seed);
    Rig r;
    std::vector<uint64_t> ids;
    for (int i = 0; i < 12; ++i) ids.push_back(r.add(500 + 100.0 * i, 100.0 * (i % 4)));   // all inside the awake ring of a player at the origin
    r.run(8, milliseconds(67));
    unsigned to_client_burst = 0, to_server_burst = 0;
    auto link = [&](unsigned& left) {
        if (left > 0) {
            --left;
            return true;
        }
        if (rng() % 100 < loss_percent) {
            left = burst > 0 ? rng() % burst : 0;
            return true;
        }
        return false;
    };
    r.lose_to_client = [&](size_t) { return link(to_client_burst); };
    r.lose_to_server = [&](size_t) { return link(to_server_burst); };
    for (size_t s = 0; s < steps; ++s) {
        const unsigned ops = rng() % 4;
        for (unsigned k = 0; k < ops; ++k) {
            const uint64_t id = ids[rng() % ids.size()];
            const world::Change kind = rng() % 3 == 0 ? world::Change::State : world::Change::Movement;
            const unsigned op = rng() % 7;
            r.note("change actor " + std::to_string(id) + " op " + std::to_string(op) + (kind == world::Change::State ? " state" : " movement"));
            switch (op) {
                case 0:
                case 1:
                    r.move_to(id, static_cast<double>(rng() % 4000), static_cast<double>(rng() % 4000), static_cast<int64_t>(rng() % 100));
                    break;
                case 2:
                    r.change(id, kind, [&](world::WorldActor& a) { a.actor.subobjects[0].obj.state.set(*a.actor.subobjects[0].obj.schema, "Level", repl::Value::i64(rng() % 50)); });
                    break;
                case 3:
                    r.change(id, kind, [&](world::WorldActor& a) { Rig::owner_of(a).add(stat_item(9000 + rng() % 40, rng() % 500)); });
                    break;
                case 4:
                    r.change(id, kind, [&](world::WorldActor& a) {
                        auto& arr = Rig::owner_of(a);
                        if (!arr.items().empty()) arr.change(arr.items()[rng() % arr.items().size()].id, stat_item(9000 + rng() % 40, rng() % 500));
                    });
                    break;
                case 5:
                    r.change(id, kind, [&](world::WorldActor& a) {
                        auto& arr = Rig::owner_of(a);
                        if (arr.items().size() > 1) arr.remove(arr.items()[rng() % arr.items().size()].id);
                    });
                    break;
                case 6: r.set_mode(id, static_cast<int>(rng() % 30)); break;
            }
        }
        r.step(milliseconds(67));
        if (r.unreadable) {
            if (why) *why = "a chain the client could not read";
            return false;
        }
        if (r.view->ledger_records() > repl::SendLedger::kMaxRecords * ids.size()) {
            if (why) *why = "the ledgers outgrew their bound";
            return false;
        }
    }
    r.quiet(150);
    std::string reason;
    if (!r.all_converged(&reason)) {
        if (why) *why = "the client did not catch up: " + reason;
        r.reg.for_each([&](const world::WorldActor& a) {
            const repl::ActorShadow* sh = r.view->shadow_of(a.id);
            const ActorModel& m = r.models[a.id];
            auto probe = [&](uint64_t oid, const repl::ObjectState& o, const repl::ObjectShadow& os, const char* name) {
                for (const auto& [h, v] : o.state.values()) {
                    const auto cv = m.objects.at(oid).values.find(h);
                    const bool client_ok = cv != m.objects.at(oid).values.end() && cv->second == v;
                    const auto sv = os.rep.sent.find(h);
                    const bool shadow_ok = sv != os.rep.sent.end() && sv->second == v;
                    if (!client_ok) std::printf("    DEBUG actor %llu %s handle %u: client_ok %d shadow_ok %d\n", static_cast<unsigned long long>(a.id), name, h, client_ok, shadow_ok);
                }
            };
            if (!sh) return;
            probe(0, a.actor.obj, sh->actor, "actor");
            for (const repl::Subobject& s : a.actor.subobjects) {
                const auto it = sh->subobjects.find(s.guid.ObjectId);
                if (it != sh->subobjects.end()) probe(s.guid.ObjectId, s.obj, it->second, s.name.c_str());
            }
        });
        if (std::getenv("NAK_TRACE")) {
            for (size_t i = 0; i < r.log.size(); ++i) std::printf("      %s\n", r.log[i].c_str());
        }
        std::printf("    DEBUG naks %zu entries %zu records %zu routes %zu in flight %zu queued %zu dropped %zu unrel %zu rel %zu server pkts %zu lost %zu\n",
                    r.view->stats().naks, r.view->stats().nak_entries, r.view->ledger_records(), r.view->routes(), r.conn.packets_in_flight(),
                    r.conn.queued_bunches(), r.conn.unreliable_dropped(), r.unreliable_chains, r.reliable_chains, r.server_packets, r.lost_to_client);
        return false;
    }
    if (r.view->ledger_records() != 0 || r.view->routes() != 0) {
        if (why) *why = "records or routes left at the end (" + std::to_string(r.view->ledger_records()) + ", " + std::to_string(r.view->routes()) + ")";
        return false;
    }
    return true;
}
// (C O A) (C_Y_B)

static void test_soak() {
    std::printf("a seeded soak: twelve NPCs, random changes, loss in bursts both ways\n");
    struct Cfg { unsigned loss, burst; };
    const Cfg cfgs[] = {{0, 1}, {3, 1}, {10, 3}, {25, 6}, {45, 10}};
    size_t failures = 0, runs = 0, naks = 0;
    for (const Cfg& c : cfgs) {
        for (unsigned s = 0; s < 30; ++s) {
            std::string why;
            ++runs;
            if (!soak_run(1000 * (c.loss + 1) + s, c.loss, c.burst, 400, &why)) {
                ++failures;
                std::printf("    seed %u (loss %u%%, burst %u): %s\n", 1000 * (c.loss + 1) + s, c.loss, c.burst, why.c_str());
            }
        }
    }
    (void)naks;
    CHECK(failures == 0, "150 runs of 400 steps: the client always catches up, nothing is left in flight");
}

int main() {
    test_movement_loss();
    test_newest_wins();
    test_stat_items();
    test_reliable_state();
    test_lost_acks();
    test_dormancy();
    test_bounds();
    test_soak();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
