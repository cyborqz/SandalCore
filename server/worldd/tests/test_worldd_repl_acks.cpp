// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_repl_acks.cpp: the replicator on top of worldd's Connection
//
//  What the two layers owe each other, which neither's own tests can show:
//
//    exports and acks   UE exports a GUID's path again until the packet that carried the export was acked
//                       (UPackageMapClient::ReceivedAck / ReceivedNak).  The replicator's ExportAcks is
//                       marked by the caller from Connection::take_ack_events: an export on channel X whose
//                       packet is lost does not make a chain on channel Y leave the path out, and the resent
//                       packet's ack does.  (The capture agrees: pkt#78's exports come again at pkt#107.)
//    atomic emission    open_actor / update_actor advance the shadow; Connection::send_chain may refuse the
//                       chain (window full).  The caller's contract (connection.h "Atomic emission"): emit
//                       on a copy of the shadow, send_chain, keep the copy only when the chain is accepted.
//                       Without it the refused update is lost for good.
//
//  The actors are the PlayerController and PlayerState of test_repl_actor_open.cpp (ch 3 pkt#22, ch 193 pkt#434),
//  cut down to a few values; the bits are checked there against the capture, here only what goes out and when.
// ============================================================================
#include "fake_client.h"
#include "net/connection.h"
#include "protocol/repl/replicator.h"
#include "protocol/schema/generated/index.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

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

static ConnectionConfig server_config() {
    ConnectionConfig c;
    c.outer = kOuter;
    c.custom_field = kCf;
    c.server_initial_seq = 14265;
    c.client_initial_seq = 15870;
    return c;
}

static ConnectionConfig client_config() {
    ConnectionConfig c = server_config();
    std::swap(c.server_initial_seq, c.client_initial_seq);
    return c;
}
// built with C-Y-B and C-O-A

/// The real pair: the server's packets reach the client (one of a batch may be lost on the way), the client's
/// acks come back.
struct Pair {
    Connection s{server_config(), t0};
    Connection cl{client_config(), t0};
    Clock::time_point t = t0;
    std::vector<InBunch> at_client;

    /// Flushes the server; the packets reach the client, except index `drop`; the acks return.  Returns the batch.
    std::vector<std::vector<uint8_t>> step(size_t drop = SIZE_MAX) {
        t += milliseconds(100);
        auto pkts = s.flush(t);
        for (size_t i = 0; i < pkts.size(); ++i) {
            if (i == drop) continue;
            for (auto& b : cl.receive(pkts[i], t)) at_client.push_back(std::move(b));
        }
        t += milliseconds(100);
        for (const auto& p : cl.flush(t)) s.receive(p, t);
        return pkts;
    }
};

// ---- the actors -------------------------------------------------------------------------------------

static const schema::ClassSchema& schema_named(const char* name) {
    const schema::ClassSchema* s = schema::gen::find_class_schema(name);
    if (!s) { std::printf("  [FAIL] no schema %s\n", name); std::exit(2); }
    return *s;
}

static FIntrepidNetworkGUID guid(uint64_t id, uint32_t server = 0, uint32_t rnd = 0) {
    FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = server;
    g.Randomizer = rnd;
    return g;
}
static FIntrepidNetworkGUID session_guid(uint64_t id) { return guid(id, 60, 0x6ee87ee4); }

constexpr uint64_t kLevelId = 16442478405498561049ull;       // PersistentLevel
constexpr uint64_t kPcArchetypeId = 3503756484819958835ull;  // Default__AoCPlayerControllerBP_C
constexpr uint64_t kGmClassId = 485698175673737329ull;       // GlobalGMCommands

static void set_value(repl::ObjectState& o, const char* path, repl::Value v) {
    if (!o.state.set(*o.schema, path, std::move(v))) {
        std::printf("  [FAIL] %s.%s refused\n", o.schema->name, path);
        g_fail++;
    }
}

static repl::ExportLink link(uint64_t id, const char* name, bool no_load, uint32_t checksum) {
    repl::ExportLink l;
    l.guid = guid(id);
    l.name = name;
    l.no_load = no_load;
    l.has_checksum = true;
    l.checksum = checksum;
    return l;
}

static repl::ExportChain level_chain() {
    return {link(kLevelId, "PersistentLevel", true, 0), link(16604466839667550161ull, "Verra_World_Master", true, 0),
            link(12923414834320654503ull, "/Game/Levels/Verra_World_Master/Verra_World_Master", true, 0)};
}

static repl::QuantVec session_location() { return repl::QuantVec{repl::QuantVec::Mode::Scaled, -7776645, 6165787, 159520}; }

/// The PlayerController of pkt#22: archetype, level and the GlobalGMCommands class in the export bunch.
static repl::Actor make_pc() {
    repl::Actor a;
    a.guid = session_guid(10341530);
    a.obj.schema = &schema_named("AoCPlayerControllerBP_C");
    set_value(a.obj, "AuthServerIDReplicated", repl::Value::u64(0));
    set_value(a.obj, "RemoteRole", repl::Value::u64(2));
    set_value(a.obj, "Role", repl::Value::u64(4));
    set_value(a.obj, "PlayerState", repl::Value(session_guid(10341528)));
    a.open.archetype = {link(kPcArchetypeId, "Default__AoCPlayerControllerBP_C", false, 0x6b62891c),
                        link(4074085207143396457ull, "/Game/ThirdPersonCPP/Blueprints/AoCPlayerControllerBP", false, 0)};
    a.open.level = level_chain();
    a.open.spawn.location = session_location();
    a.open.player_controller = repl::PlayerControllerOpen{0, 0};
    repl::Subobject gm;   // a dynamic subobject: spawned from its class, which is exported with its package
    gm.guid = session_guid(10341532);
    gm.dynamic_class = repl::ExportChain{link(kGmClassId, "GlobalGMCommands", false, 0xcaaaee3e),
                                         link(158953490572197689ull, "/Script/GameSystemsPlugin", false, 0)};
    a.subobjects.push_back(std::move(gm));
    return a;
}

/// The PlayerState of pkt#434: its own archetype, and the same level as the PlayerController's.
static repl::Actor make_ps() {
    repl::Actor a;
    a.guid = session_guid(10341528);
    a.obj.schema = &schema_named("AoCPlayerStateBP_C");
    set_value(a.obj, "AuthServerIDReplicated", repl::Value::u64(0));
    set_value(a.obj, "RemoteRole", repl::Value::u64(1));
    set_value(a.obj, "Owner", repl::Value(session_guid(10341530)));
    set_value(a.obj, "Role", repl::Value::u64(4));
    set_value(a.obj, "PlayerId", repl::Value::i64(1874));
    a.open.archetype = {link(16037722909354717791ull, "Default__AoCPlayerStateBP_C", false, 0xf4f44182),
                        link(1938806816082851941ull, "/Game/ThirdPersonCPP/Blueprints/AoCPlayerStateBP", false, 0)};
    a.open.level = level_chain();
    a.open.spawn.location = session_location();
    return a;
}

static Chain to_chain(uint32_t channel, bool open, repl::Emission e) {
    Chain c;
    c.channel = channel;
    c.open = open;
    c.exports = std::move(e.exports);
    c.payload = std::move(e.payload);
    return c;
}

static bool exported(const repl::Emission& e, uint64_t object_id) {
    for (const auto& g : e.exported)
        if (g.ObjectId == object_id) return true;
    return false;
}

static size_t export_bits(const repl::Emission& e) {
    size_t n = 0;
    for (const auto& b : e.exports) n += b.bit_pos();
    return n;
}

// ---- exports and acks -----------------------------------------------------------------------------------

/// The caller's side of ack reporting: per chain, the GUIDs each of its export bunches wrote
/// (Emission::exported_by_bunch); an event that says export part i was delivered marks the GUIDs of bunch i, as UE
/// marks a GUID when the packet that carried its export is acked.  A lost part marks nothing: a reliable part comes
/// again and is reported when that packet is acked (the events of a lost part are final only for an unreliable chain).
struct ExportBook {
    std::map<ChainId, std::vector<std::vector<FIntrepidNetworkGUID>>> chains;
    repl::ExportAcks acks;

    void note(ChainId id, const repl::Emission& e) { chains[id] = e.exported_by_bunch; }
    void apply(const std::vector<AckEvent>& events) {
        for (const auto& ev : events) {
            auto it = chains.find(ev.chain);
            if (it == chains.end() || !ev.export_part || !ev.delivered || ev.part >= it->second.size()) continue;
            acks.mark(it->second[ev.part]);
        }
    }
};

static void test_export_acks_follow_the_packets() {
    std::printf("exports are marked when the packet that carried them is acked\n");
    Pair p;
    ExportBook book;

    // Channel 3: the PlayerController opens, exporting its archetype, the level and the GlobalGMCommands class.
    repl::Actor pc = make_pc();
    repl::ActorShadow shadow_x;
    const auto x = repl::open_actor(pc, shadow_x, book.acks);
    CHECK(x && x->exports.size() == 1 && exported(*x, kLevelId) && exported(*x, kPcArchetypeId) && exported(*x, kGmClassId),
          "X, the PlayerController's open: one export bunch with the level, the archetype and the class");
    if (!x) return;
    const ChainId idx = p.s.send_chain(to_chain(3, true, *x));
    book.note(idx, *x);
    CHECK(idx != 0 && !book.acks.acked(guid(kLevelId)), "queued: nothing is acknowledged by queueing (UE: not before the ack)");
    const auto lost = p.step(0);   // X's packet does not arrive
    CHECK(lost.size() == 1 && p.at_client.empty(), "X goes out in one packet, which is lost");
// C:Y:B : C | O | A

    // Channel 193: the PlayerState opens, built while X's fate is still unknown: the level is exported in full again.
    repl::Actor ps = make_ps();
    repl::ActorShadow shadow_y;
    const auto y = repl::open_actor(ps, shadow_y, book.acks);
    repl::ActorShadow baseline_shadow;
    const auto baseline = repl::open_actor(ps, baseline_shadow, repl::ExportAcks{});
    CHECK(y && baseline && exported(*y, kLevelId) && export_bits(*y) == export_bits(*baseline),
          "Y, the PlayerState's open, built after X was queued: still exports the level with its full path");
    if (!y) return;
    const ChainId idy = p.s.send_chain(to_chain(193, true, *y));
    book.note(idy, *y);
    p.step();   // Y arrives; the client's ack shows X's packet missing
    const auto events = p.s.take_ack_events();
    bool x_lost = false, y_delivered = false;
    for (const auto& ev : events) {
        x_lost = x_lost || (ev.chain == idx && ev.export_part && !ev.delivered);
        y_delivered = y_delivered || (ev.chain == idy && ev.export_part && ev.delivered);
    }
    CHECK(x_lost && y_delivered, "the events say X's export part was lost and Y's delivered");
    book.apply(events);
    CHECK(book.acks.acked(guid(kLevelId)) && !book.acks.acked(guid(kGmClassId)) && !book.acks.acked(guid(kPcArchetypeId)),
          "marking follows them: the level (Y's, delivered) is acknowledged, X's archetype and class (lost) are not");

    // The PlayerController opens again (a third channel): its class and archetype are still exported with paths, the
    // level is not.
    repl::ActorShadow shadow_z;
    const auto z = repl::open_actor(pc, shadow_z, book.acks);
    CHECK(z && z->exports.size() == 1 && exported(*z, kGmClassId) && exported(*z, kPcArchetypeId) && !exported(*z, kLevelId) &&
              export_bits(*z) < export_bits(*x),
          "an open built now still exports what X's lost packet carried, and leaves out the delivered level");

    // X's packet is sent again (same chSeq); its ack is the event that marks X's exports.
    p.step();
    const auto later = p.s.take_ack_events();
    bool x_delivered = false;
    for (const auto& ev : later) x_delivered = x_delivered || (ev.chain == idx && ev.export_part && ev.delivered);
    CHECK(x_delivered && p.at_client.size() == 2, "the resent packet arrives and is acknowledged: the client has X's open");
    book.apply(later);
    repl::ActorShadow shadow_w;
    const auto w = repl::open_actor(pc, shadow_w, book.acks);
    CHECK(w && w->exports.empty() && w->payload.bit_pos() == x->payload.bit_pos(),
          "after that an open of the same actor has no export bunch at all, the same payload");
}

// ---- atomic emission ------------------------------------------------------------------------------------------

static bool hidden_update(const repl::Emission& e, const repl::Actor& pc) {
    const auto views = repl::read_content_blocks(e.payload.data(), e.payload.bit_pos(), 0);
    size_t pos = views && !views->empty() ? (*views)[0].payload_pos : 0;
    const auto st = views && !views->empty() ? repl::read_rep_block(*pc.obj.schema, e.payload.data(), e.payload.bit_pos(), pos)
                                              : std::nullopt;
    return st && st->get(3) && *st->get(3) == repl::Value::boolean(true);   // handle 3: bHidden
}

static void test_shadow_contract() {
    std::printf("an update the Connection refuses is not lost (check before you commit)\n");
    Pair p;
    repl::ExportAcks acks;
    repl::Actor pc = make_pc();
    repl::ActorShadow shadow;
    bool failed = false;
    const auto open = repl::open_actor(pc, shadow, acks, &failed);
    CHECK(open && !failed && p.s.send_chain(to_chain(21, true, *open)) != 0, "the PlayerController's channel opens");
    if (!open) return;
    while (p.s.unacked_reliable(21) < Connection::kMaxUnackedReliable) {
        Chain filler;
        filler.channel = 21;
        filler.payload.write_bits(1, 8);
        if (!p.s.send_chain(filler)) break;
    }
    CHECK(p.s.unacked_reliable(21) == Connection::kMaxUnackedReliable, "the channel's window is full: 256 bunches in flight");

    set_value(pc.obj, "bHidden", repl::Value::boolean(true));

    {   // committed before the send: the refusal loses the update for good
        repl::ActorShadow committed = shadow;
        const auto e = repl::update_actor(pc, committed, acks, &failed);
        CHECK(e && !failed && hidden_update(*e, pc), "the update carries bHidden");
        if (!e) return;
        const Chain c = to_chain(21, false, *e);
        CHECK(!p.s.can_send_chain(c) && p.s.send_chain(c) == 0, "the window is full: the chain is refused");
        CHECK(!repl::update_actor(pc, committed, acks, &failed) && !failed,
              "a shadow kept after that refusal has nothing left to send: the client would never get bHidden");
    }

    // The contract: emit on a copy, keep the copy only when the chain is accepted.
    repl::ActorShadow next = shadow;
    auto e = repl::update_actor(pc, next, acks, &failed);
    if (!e) return;
    Chain c = to_chain(21, false, std::move(*e));
    if (p.s.can_send_chain(c)) shadow = std::move(next);
    CHECK(!p.s.can_send_chain(c), "again refused: the copy is dropped, the shadow stays as it was");

    p.step();
    p.step();
    CHECK(p.s.unacked_reliable(21) == 0, "the acks free the window");

    repl::ActorShadow next2 = shadow;
    const auto e2 = repl::update_actor(pc, next2, acks, &failed);
    CHECK(e2 && !failed && hidden_update(*e2, pc), "the same update is built again from the kept shadow: it still carries bHidden");
    if (!e2) return;
    Chain c2 = to_chain(21, false, *e2);
    CHECK(p.s.can_send_chain(c2), "and it is accepted");
    const bool sent = p.s.send_chain(c2) != 0;
    if (sent) shadow = std::move(next2);
    CHECK(sent && !repl::update_actor(pc, shadow, acks, &failed) && !failed,
          "now the shadow is committed: there is nothing more to send");
    p.step();
    CHECK(!p.at_client.empty() && p.at_client.back().channel == 21 && p.at_client.back().bits == c2.payload.bit_pos(),
          "and the client receives it");
}

int main() {
    test_export_acks_follow_the_packets();
    test_shadow_contract();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
