// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_trade.cpp: player-to-player trading (window, offers, validation, cancels)
// ============================================================================
#include "test_registry.h"
#include "shared_world_data.h"
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "bootstrap/bootstrap.h"
#include "content/content_db.h"
#include "items/inventory.h"
#include "items/wallet.h"
#include "net/connection.h"
#include "protocol/repl/fast_array.h"
#include "protocol/repl/item_rpc.h"
#include "protocol/repl/rep_block.h"
#include "protocol/repl/replicator.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/rpc_fields.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/repl/trade_rpc.h"
#include "protocol/schema/generated/index.h"
#include "session/player_session.h"
#include "session/player_world.h"
#include "trade/trade_book.h"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
namespace fs = std::filesystem;
using emit::BunchWriter;
using svc::ItemContainer;
using std::chrono::milliseconds;
using std::chrono::seconds;

static std::ostringstream g_log;
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static const std::string kAliceId = "bbec5c86eb101fe5f38c87f0fa22e7d0";
static const std::string kBobId   = "bbec5c86eb101fe5f38c87f0fa22e7d1";
static const std::string kCarolId = "bbec5c86eb101fe5f38c87f0fa22e7d2";
static const std::string kMachine = "DESKTOP-TRADE-TEST-0123456789ABCDEF";
static const Clock::time_point t0{seconds(5000)};
static constexpr uint32_t kServerId = 60;
static constexpr uint32_t kRandomizer = 0x6ee87ee4u;

// Real item records from dist/content/content.db
static constexpr int64_t kRecTradeableStack    = 107048815515256ll;      // TestItem (Tradeable=true, max_stack=5)
static constexpr int64_t kRecTradeableAlt1     = 107123883923064ll;      // Tradeable=true
static constexpr int64_t kRecTradeableAlt2     = 107445321160312ll;      // Tradeable=true
static constexpr int64_t kRecTradeableWearable = 109229507958392ll;      // Gear_Armor_Light_AcademicAegis_Head (slot 1, Tradeable=true)
static constexpr int64_t kRecUntradeable1      = 107411158881912ll;      // Mount_Flying_Wyrmling_0 (Tradeable=false)
static constexpr int64_t kRecUntradeable2      = 108254765143672ll;      // Tradeable=false
static constexpr int64_t kRecUntradeable3      = 108367665124984ll;      // Tradeable=false
static constexpr int64_t kRecTradeableResource = 109162169652921ll;      // 1x1 resource (Tradeable=true)
static constexpr int64_t kRecResourceBag       = 6064631233994358840ll;  // Bag_Resource_Crier (6x4, slot 23)
static constexpr int64_t kRecQuestItem         = 6064630639305228288ll;  // Quest item
static constexpr int64_t kRecCargoCrate        = 6064630197684338693ll;  // Cargo crate (slot 53)

static const schema::ClassSchema& class_named(const char* name) {
    const schema::ClassSchema* c = gen::find_class_schema(name);
    if (!c) {
        std::printf("  [FAIL] missing class schema %s\n", name);
        std::exit(2);
    }
    return *c;
}

static std::string real_content_db_path() {
    if (const char* env = std::getenv("AOC_CONTENT_DB"); env && *env) return env;
#ifdef TRADE_CONTENT_DB_PATH
    return TRADE_CONTENT_DB_PATH;
#else
    return {};
#endif
}

static std::string item_guid_n(uint32_t n) {
    char buf[33];
    std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x", 0xB1000000u + n, 0x2000u + n, 0x3000u + n, n | 1u);
    return buf;
}

static svc::InventoryItem make_item(ItemContainer c, int slot, int64_t record, uint32_t n, int32_t quantity = 1, int32_t quality = 1) {
    svc::InventoryItem it;
    it.container = c;
    it.slot = slot;
    it.item_record_id = record;
    it.instance_guid = item_guid_n(n);
    it.quantity = quantity;
    it.quality = quality;
    it.durability = 10.0f;
    it.created_at_ms = 1'771'299'474'000ll;
    return it;
}

static svc::Character make_trader(const std::string& id, const std::string& name, double x, double y, double z,
                                  int64_t copper, std::vector<svc::InventoryItem> items) {
    svc::Character c;
    c.id = id;
    c.account_id = 10;
    c.name = name;
    c.race_id = 0;
    c.gender_id = 0;
    c.class_id = 2;
    c.archetype_id = 17747;
    c.class_name = "Bard";
    c.appearance_json = cyb::kAppearanceJsonb;
    c.level = 1;
    c.has_position = true;
    c.position = svc::Position{x, y, z, 0.0f};
    c.wallet.push_back(svc::WalletEntry{items::kCopperCurrency, copper});
    c.items = std::move(items);
    return c;
}

static std::vector<svc::InventoryItem> default_trader_items(uint32_t base_n) {
    std::vector<svc::InventoryItem> out;
    // Worn resource bag in slot 23 so Resource0 has a 6x4 grid
    out.push_back(make_item(ItemContainer::Equipment, items::equip::ResourceBag0, kRecResourceBag, base_n + 1, 1));
    // Worn helmet in slot 1 (tests equipped item rejection)
    out.push_back(make_item(ItemContainer::Equipment, items::equip::Helmet, kRecTradeableWearable, base_n + 2, 1));
    // Backpack items
    out.push_back(make_item(ItemContainer::Backpack, 0, kRecTradeableStack, base_n + 10, 5, 1));
    out.push_back(make_item(ItemContainer::Backpack, 1, kRecTradeableWearable, base_n + 11, 1, 1));
    out.push_back(make_item(ItemContainer::Backpack, 2, kRecUntradeable1, base_n + 12, 1, 1));
    out.push_back(make_item(ItemContainer::Backpack, 3, kRecCargoCrate, base_n + 13, 1, 1));
    // Resource0 material item
    out.push_back(make_item(ItemContainer::Resource0, 0, kRecTradeableResource, base_n + 20, 10, 1));
    // Quest storage item
    out.push_back(make_item(ItemContainer::Quest, 0, kRecQuestItem, base_n + 30, 1, 1));
    return out;
}

static BunchWriter pc_rpc_bunch(const char* rpc_name, const repl::RpcParams& params) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    repl::Actor a;
    a.guid = emit::FIntrepidNetworkGUID::from_u64(4);
    a.obj.schema = &pc;
    const auto em = repl::call_rpc(a, nullptr, rpc_name, params, {}, repl::ExportAcks{});
    if (!em) {
        std::printf("  [FAIL] could not encode PC RPC %s\n", rpc_name);
        return BunchWriter();
    }
    return em->payload;
}

static BunchWriter pawn_rpc_bunch(const char* rpc_name, const repl::RpcParams& params) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    repl::Actor a;
    a.guid = emit::FIntrepidNetworkGUID::from_u64(8);
    a.obj.schema = &pawn;
    const auto em = repl::call_rpc(a, nullptr, rpc_name, params, {}, repl::ExportAcks{});
    if (!em) {
        std::printf("  [FAIL] could not encode Pawn RPC %s\n", rpc_name);
        return BunchWriter();
    }
    return em->payload;
}

struct RecordingSink : PositionSink {
    struct SavedInv {
        std::string id;
        std::vector<svc::InventoryItem> items;
    };
    std::vector<svc::Position> positions;
    std::vector<SavedInv> inventories;
    std::vector<std::pair<std::string, svc::Progress>> progress;
    void save(const std::string&, const svc::Position& p) override { positions.push_back(p); }
    void save_inventory(const std::string& id, std::vector<svc::InventoryItem> items) override { inventories.push_back({id, std::move(items)}); }
    void save_progress(const std::string& id, svc::Progress p) override { progress.emplace_back(id, std::move(p)); }
};

struct TradeMirror {
    bs::SessionGuids guids;
    const schema::ClassSchema* pc_class = nullptr;
    const schema::ClassSchema* info_class = nullptr;
    std::vector<repl::ClientTradeRequest>         requests;
    std::vector<repl::ClientTradeRequestResponse> responses;
    std::vector<repl::ClientTradeBegin>           begins;
    std::vector<repl::ClientTradeUpdate>          updates;
    std::vector<repl::ClientTradeClose>           closes;
    std::vector<std::string>                      callouts;
    std::vector<bool>                             trade_in_progress_history;
    bool                                          trade_in_progress = false;
    size_t                                        unreadable = 0;

    void clear_events() {
        requests.clear();
        responses.clear();
        begins.clear();
        updates.clear();
        closes.clear();
        callouts.clear();
        trade_in_progress_history.clear();
    }

    void absorb(const fake::Delivered& d, uint32_t pc_ch, uint32_t pawn_ch) {
        if (d.open || d.close || d.content.bit_pos() == 0) return;
        const auto views = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
        if (!views) {
            ++unreadable;
            return;
        }
        for (const repl::BlockView& v : *views) {
            const size_t end = v.payload_pos + v.payload_bits;
            size_t p = v.payload_pos;
            if (d.channel == pc_ch && v.header.is_actor) {
                if (v.header.has_rep_layout) continue;
                if (pc_class) {
                    size_t rpc_pos = p;
                    const auto calls = repl::read_rpc_fields(*pc_class, d.content.data(), end, rpc_pos);
                    if (calls && !calls->empty()) {
                        for (const repl::RpcCall& c : *calls) {
                            if (!c.decoded || !c.rpc) continue;
                            const std::string_view n(c.rpc->name);
                            if (n == "ClientRequestTrade") {
                                if (const auto r = repl::parse_client_request_trade(c)) requests.push_back(*r);
                                else ++unreadable;
                            } else if (n == "ClientProcessTradeRequestResponse") {
                                if (const auto r = repl::parse_client_trade_response(c)) responses.push_back(*r);
                                else ++unreadable;
                            } else if (n == "ClientBeginTrade") {
                                if (const auto r = repl::parse_client_begin_trade(c)) begins.push_back(*r);
                                else ++unreadable;
                            } else if (n == "ClientUpdateTrade") {
                                if (const auto r = repl::parse_client_update_trade(c)) updates.push_back(*r);
                                else ++unreadable;
                            } else if (n == "ClientCloseTrade") {
                                if (const auto r = repl::parse_client_close_trade(c)) closes.push_back(*r);
                                else ++unreadable;
                            }
                        }
                    } else {
                        size_t arr_pos = p;
                        const auto layout = repl::fast_array_layout(*pc_class, "CalloutQueueReplication");
                        const auto fields = repl::read_fast_array_fields(*pc_class, d.content.data(), end, arr_pos);
                        if (layout && fields) {
                            for (const repl::FastArrayField& f : *fields) {
                                if (f.field != layout->field) continue;
                                for (const auto& ch : f.delta.changed) {
                                    for (const auto& [h, val] : ch.values.values) {
                                        if (const auto* s = val.get_if<std::string>(); s && !s->empty()) callouts.push_back(*s);
                                    }
                                }
                            }
                        }
                    }
                }
            } else if (d.channel == pawn_ch && !v.header.is_actor && v.header.object == guids.base_character_info && info_class) {
                if (v.header.has_rep_layout) {
                    const auto st = repl::read_rep_block(*info_class, d.content.data(), end, p);
                    if (st) {
                        if (const repl::Value* tv = st->get(*info_class, "bTradeInProgress")) {
                            if (const bool* b = tv->get_if<bool>()) {
                                trade_in_progress = *b;
                                trade_in_progress_history.push_back(*b);
                            }
                        }
                    }
                }
            }
        }
    }
};

struct TradeClient {
    Connection conn;
    fake::Client client;
    std::unique_ptr<PlayerSession> s;
    uint32_t pc_ch = 0, pawn_ch = 0;
    std::vector<fake::Delivered> got;
    size_t mirrored = 0;
    TradeMirror mirror;

    TradeClient(PlayerWorld& world, const svc::Character& rec, Clock::time_point now, double world_time, uint32_t salt)
        : conn(ConnectionConfig{conn::OuterHeader{0x500c7696u + salt, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, static_cast<uint8_t>(0x5d + salt)},
                                static_cast<uint16_t>(14265 + salt * 10), static_cast<uint16_t>(15870 + salt * 10)}, now),
          client(conn::OuterHeader{0x500c7696u + salt, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, static_cast<uint8_t>(0x5d + salt)},
                 static_cast<uint16_t>(14265 + salt * 10), static_cast<uint16_t>(15870 + salt * 10)) {
        std::string err;
        s = PlayerSession::join(conn, world, rec, bs::LoginIdentity{kMachine, kMachine}, world_time, err);
        if (!s) {
            std::printf("  [FAIL] PlayerSession::join failed for %s: %s\n", rec.name.c_str(), err.c_str());
            std::exit(2);
        }
        pc_ch = s->channel_of(bs::Role::PlayerController);
        pawn_ch = s->channel_of(bs::Role::Pawn);
        mirror.guids = s->bootstrap().setup().guids;
        mirror.pc_class = &class_named("AoCPlayerControllerBP_C");
        const repl::Subobject* info = s->bootstrap().pawn().find(mirror.guids.base_character_info);
        mirror.info_class = info ? info->obj.schema : nullptr;
    }

    repl::GuidValue guid() const { return trade::to_guid_value(s->bootstrap().setup().character_guid); }
    trade::PlayerGuid guid_words() const { return s->bootstrap().setup().character_guid; }

    void flush(Clock::time_point now) {
        for (auto& pkt : conn.flush(now)) {
            const auto r = client.receive(pkt);
            for (const auto& d : r.delivered) got.push_back(d);
        }
        for (; mirrored < got.size(); ++mirrored) mirror.absorb(got[mirrored], pc_ch, pawn_ch);
    }

    void deliver(std::vector<fake::Bunch> bunches, Clock::time_point now) {
        for (const auto& pkt : client.packets(std::move(bunches))) {
            const std::vector<InBunch> in = conn.receive(pkt, now);
            s->take_acks(conn);
            for (const InBunch& b : in) s->on_bunch(conn, b, now);
            s->end_of_datagram(conn);
        }
    }

    void tick(Clock::time_point now) {
        s->tick(conn, now);
        flush(now);
        s->take_acks(conn);
        deliver({}, now);
    }

    void send_pc(const char* rpc_name, const repl::RpcParams& params, Clock::time_point now) {
        const BunchWriter w = pc_rpc_bunch(rpc_name, params);
        constexpr size_t kChunkBits = 6000;
        if (w.bit_pos() <= kChunkBits) {
            deliver({client.actor(pc_ch, w.data(), w.bit_pos(), true)}, now);
            return;
        }
        std::vector<fake::Bunch> parts;
        for (size_t off = 0; off < w.bit_pos(); off += kChunkBits) {
            const size_t n = std::min(kChunkBits, w.bit_pos() - off);
            BunchWriter slice;
            slice.write_bit_range(w.data(), off, n);
            fake::Bunch b = client.actor(pc_ch, slice.data(), slice.bit_pos(), true);
            b.header.partial = true;
            b.header.partial_initial = (off == 0);
            b.header.partial_final = (off + n >= w.bit_pos());
            parts.push_back(std::move(b));
        }
        deliver(std::move(parts), now);
    }

    void send_pawn(const char* rpc_name, const repl::RpcParams& params, Clock::time_point now) {
        const BunchWriter w = pawn_rpc_bunch(rpc_name, params);
        deliver({client.actor(pawn_ch, w.data(), w.bit_pos(), true)}, now);
    }

    void phase_in(Clock::time_point now) {
        send_pawn("ServerClientIsReadyToPhaseIn", repl::RpcParams{}, now);
    }

    void send_request_trade(const repl::GuidValue& target, Clock::time_point now) {
        const schema::RpcSchema* rpc = class_named("AoCPlayerControllerBP_C").rpc("ServerRequestTrade");
        const auto params = rpc ? repl::server_request_trade_params(*rpc, target) : std::nullopt;
        if (params) send_pc("ServerRequestTrade", *params, now);
    }

    void send_trade_response(const repl::GuidValue& requester, uint8_t response, Clock::time_point now) {
        const schema::RpcSchema* rpc = class_named("AoCPlayerControllerBP_C").rpc("ServerRequestTradeResponse");
        const auto params = rpc ? repl::server_trade_response_params(*rpc, requester, response) : std::nullopt;
        if (params) send_pc("ServerRequestTradeResponse", *params, now);
    }

    void send_update_items(const repl::GuidValue& trade_guid, const repl::TradeOffer& offer, Clock::time_point now) {
        const schema::RpcSchema* rpc = class_named("AoCPlayerControllerBP_C").rpc("ServerUpdateTradeItems");
        const auto params = rpc ? repl::server_update_trade_items_params(*rpc, trade_guid, offer) : std::nullopt;
        if (params) send_pc("ServerUpdateTradeItems", *params, now);
    }

    void send_transaction(const repl::GuidValue& partner, const repl::GuidValue& trade_guid, uint8_t req, Clock::time_point now) {
        const schema::RpcSchema* rpc = class_named("AoCPlayerControllerBP_C").rpc("ServerUpdateTradeTransaction");
        const auto params = rpc ? repl::server_update_trade_transaction_params(*rpc, partner, trade_guid, req) : std::nullopt;
        if (params) send_pc("ServerUpdateTradeTransaction", *params, now);
    }
};

struct TradeWorldFixture {
    RecordingSink sink;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;
    Clock::time_point t = t0;

    explicit TradeWorldFixture(const items::ItemCatalog* catalog_override = nullptr) {
        const std::string path = real_content_db_path();
        shared_world::Entry& entry = shared_world::get(path);
        if (!entry.db || !entry.data) {
            std::printf("  [FAIL] real content.db could not be opened at %s: %s\n", path.c_str(), entry.problem.c_str());
            std::exit(2);
        }
        world = std::make_unique<PlayerWorld>(entry.data, entry.problem, bs::GameStateMode::Tier2, kServerId, guids);
        world->set_position_sink(&sink);
        if (catalog_override) world->set_item_catalog(catalog_override);
    }

    double now_sec() const { return std::chrono::duration<double>(t - t0).count(); }

    void step(std::initializer_list<TradeClient*> clients, milliseconds d = milliseconds(50)) {
        t += d;
        world->tick_world(now_sec());
        for (TradeClient* c : clients) {
            if (c && c->s) c->tick(t);
        }
    }

    void settle(std::initializer_list<TradeClient*> clients, int rounds = 15) {
        for (int i = 0; i < rounds; ++i) step(clients, milliseconds(20));
    }

    void settle_join_and_phase_in(std::initializer_list<TradeClient*> clients) {
        settle(clients, 40);
        for (TradeClient* c : clients) c->phase_in(t);
        settle(clients, 20);
        for (TradeClient* c : clients) c->mirror.clear_events();
        sink.inventories.clear();
        sink.positions.clear();
        sink.progress.clear();
    }

    bool observer_trade_in_progress(const TradeClient& c) const {
        const uint64_t pawn_id = c.s->bootstrap().setup().guids.pawn.ObjectId;
        const world::WorldActor* wa = world->registry().find(pawn_id);
        if (!wa) return false;
        const repl::Subobject* base = wa->actor.find(c.s->bootstrap().setup().guids.base_character_info);
        if (!base || !base->obj.schema) return false;
        const repl::Value* v = base->obj.state.get(*base->obj.schema, "bTradeInProgress");
        const bool* b = v ? v->get_if<bool>() : nullptr;
        return b && *b;
    }
};

// ============================================================================
//  1. Wire codecs round-trip (all 9 trade RPCs + copper payloads)
// ============================================================================
static void trade_wire_codecs() {
    std::printf("1. wire codecs: round-trip all 9 trade RPCs and copper payloads\n");
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const auto roundtrip = [&](const char* name, const repl::RpcParams& params) -> std::optional<repl::RpcCall> {
        repl::Actor a;
        a.guid = emit::FIntrepidNetworkGUID::from_u64(4);
        a.obj.schema = &pc;
        const auto em = repl::call_rpc(a, nullptr, name, params, {}, repl::ExportAcks{});
        if (!em) return std::nullopt;
        const auto blocks = repl::read_content_blocks(em->payload.data(), em->payload.bit_pos(), 0);
        if (!blocks || blocks->size() != 1) return std::nullopt;
        const repl::BlockView& b = blocks->front();
        size_t pos = b.payload_pos;
        const auto calls = repl::read_rpc_fields(pc, em->payload.data(), b.payload_pos + b.payload_bits, pos);
        if (!calls || calls->size() != 1) return std::nullopt;
        return calls->front();
    };

    const repl::GuidValue gA{0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff01u};
    const repl::GuidValue gB{0x22334455u, 0x66778899u, 0xaabbccddu, 0xeeff0012u};
    const repl::GuidValue gT{0x77771111u, 0x88882222u, 0x99993333u, 0xaaaa4444u};

    // ServerRequestTrade
    {
        const auto p = repl::server_request_trade_params(*pc.rpc("ServerRequestTrade"), gB);
        const auto c = p ? roundtrip("ServerRequestTrade", *p) : std::nullopt;
        const auto parsed = c ? repl::parse_trade_request(*c) : std::nullopt;
        CHECK(parsed && parsed->character_guid == gB, "ServerRequestTrade round-trips");
    }
    // ServerRequestTradeResponse (0=ACCEPT default-omitted and 7=REJECT_CORRUPTED)
    {
        bool ok_all = true;
        for (uint8_t r : {uint8_t{0}, uint8_t{1}, uint8_t{3}, uint8_t{7}}) {
            const auto p = repl::server_trade_response_params(*pc.rpc("ServerRequestTradeResponse"), gA, r);
            const auto c = p ? roundtrip("ServerRequestTradeResponse", *p) : std::nullopt;
            const auto parsed = c ? repl::parse_trade_request_response(*c) : std::nullopt;
            if (!parsed || parsed->target_character_guid != gA || parsed->response != r) ok_all = false;
        }
        CHECK(ok_all, "ServerRequestTradeResponse round-trips 0=ACCEPT through 7=REJECT_CORRUPTED");
    }
    // ServerUpdateTradeItems & ClientUpdateTrade with items + copper (0, 1, 10000, 150000)
    {
        const svc::InventoryItem sample1 = make_item(ItemContainer::Backpack, 0, kRecTradeableStack, 50, 3, 25);
        const svc::InventoryItem sample2 = make_item(ItemContainer::Resource0, 0, kRecTradeableResource, 51, 10, 1);
        bool srv_ok = true, cli_ok = true;
        int32_t ver = 1;
        for (int64_t copper : {int64_t{0}, int64_t{1}, int64_t{10000}, int64_t{150000}}) {
            repl::TradeOffer offer;
            if (copper > 0) {
                offer.items.push_back(items::to_instance(sample1));
                offer.items.push_back(items::to_instance(sample2));
            }
            offer.money = repl::trade_money_container(copper);
            offer.version = ver++;
            {
                const auto p = repl::server_update_trade_items_params(*pc.rpc("ServerUpdateTradeItems"), gT, offer);
                const auto c = p ? roundtrip("ServerUpdateTradeItems", *p) : std::nullopt;
                const auto parsed = c ? repl::parse_trade_items_update(*c) : std::nullopt;
                if (!parsed || parsed->trade_guid != gT || parsed->offer != offer) srv_ok = false;
            }
            {
                const auto p = repl::client_update_trade_params(*pc.rpc("ClientUpdateTrade"), gA, offer);
                const auto c = p ? roundtrip("ClientUpdateTrade", *p) : std::nullopt;
                const auto parsed = c ? repl::parse_client_update_trade(*c) : std::nullopt;
                if (!parsed || parsed->character_guid != gA || parsed->offer != offer) cli_ok = false;
            }
        }
        CHECK(srv_ok, "ServerUpdateTradeItems round-trips items + FCurrencyContainer (0/1/10000/150000 copper) + Version");
        CHECK(cli_ok, "ClientUpdateTrade round-trips items + FCurrencyContainer (0/1/10000/150000 copper) + Version");
    }
    // ServerUpdateTradeTransaction (0..4 and zero partner missing-pawn fallback)
    {
        bool ok_all = true;
        for (uint8_t req = 0; req <= 4; ++req) {
            const repl::GuidValue partner = (req == 1) ? repl::GuidValue{} : gB;
            const auto p = repl::server_update_trade_transaction_params(*pc.rpc("ServerUpdateTradeTransaction"), partner, gT, req);
            const auto c = p ? roundtrip("ServerUpdateTradeTransaction", *p) : std::nullopt;
            const auto parsed = c ? repl::parse_trade_transaction(*c) : std::nullopt;
            if (!parsed || parsed->trade_character_guid != partner || parsed->trade_guid != gT || parsed->request != req) ok_all = false;
        }
        CHECK(ok_all, "ServerUpdateTradeTransaction round-trips 0..4 and zero-partner fallback");
    }
    // ClientRequestTrade, ClientProcessTradeRequestResponse, ClientBeginTrade, ClientCloseTrade
    {
        const auto p1 = repl::client_request_trade_params(*pc.rpc("ClientRequestTrade"), gA);
        const auto c1 = p1 ? roundtrip("ClientRequestTrade", *p1) : std::nullopt;
        const auto r1 = c1 ? repl::parse_client_request_trade(*c1) : std::nullopt;
        CHECK(r1 && r1->character_guid == gA, "ClientRequestTrade round-trips");

        bool resp_ok = true;
        for (uint8_t code = 0; code <= 7; ++code) {
            const auto p2 = repl::client_trade_response_params(*pc.rpc("ClientProcessTradeRequestResponse"), gB, code);
            const auto c2 = p2 ? roundtrip("ClientProcessTradeRequestResponse", *p2) : std::nullopt;
            const auto r2 = c2 ? repl::parse_client_trade_response(*c2) : std::nullopt;
            if (!r2 || r2->character_guid != gB || r2->response != code) resp_ok = false;
        }
        CHECK(resp_ok, "ClientProcessTradeRequestResponse round-trips codes 0..7");

        const auto p3 = repl::client_begin_trade_params(*pc.rpc("ClientBeginTrade"), gT, gB);
        const auto c3 = p3 ? roundtrip("ClientBeginTrade", *p3) : std::nullopt;
        const auto r3 = c3 ? repl::parse_client_begin_trade(*c3) : std::nullopt;
        CHECK(r3 && r3->trade_request_guid == gT && r3->target_guid == gB, "ClientBeginTrade round-trips");

        bool close_ok = true;
        for (uint8_t st : {uint8_t{0}, uint8_t{1}}) {
            const auto p4 = repl::client_close_trade_params(*pc.rpc("ClientCloseTrade"), st);
            const auto c4 = p4 ? roundtrip("ClientCloseTrade", *p4) : std::nullopt;
            const auto r4 = c4 ? repl::parse_client_close_trade(*c4) : std::nullopt;
            if (!r4 || r4->status != st) close_ok = false;
        }
        CHECK(close_ok, "ClientCloseTrade round-trips 0=FAILURE and 1=SUCCESS");
    }
}
AOC_REGISTER_TEST(trade_wire_codecs);

// ============================================================================
//  2. ContentDb & ContentItemCatalog Tradeable flag (real content.db + minimal fixture)
// ============================================================================
static void trade_content_db_tradeable() {
    std::printf("2. content.db ItemRecordBase.Tradeable flag (real content.db + minimal fixture)\n");
    const std::string real_path = real_content_db_path();
    std::string err;
    const auto real_db = content::ContentDb::open(real_path, err);
    CHECK(real_db.has_value(), "real content.db opens for Tradeable verification");
    if (real_db) {
        items::ContentItemCatalog cat(*real_db);
        bool all_true = true;
        for (int64_t id : {kRecTradeableStack, kRecTradeableAlt1, kRecTradeableAlt2, kRecTradeableWearable, kRecTradeableResource}) {
            const auto rec = real_db->item(id, err);
            const auto info = cat.info(id);
            if (!rec || !rec->tradeable || !info || !info->tradeable) all_true = false;
        }
        CHECK(all_true, "real content.db Tradeable=true records read tradeable==true in ContentDb and ItemCatalog");

        bool all_false = true;
        for (int64_t id : {kRecUntradeable1, kRecUntradeable2, kRecUntradeable3}) {
            const auto rec = real_db->item(id, err);
            const auto info = cat.info(id);
            if (!rec || rec->tradeable || !info || info->tradeable) all_false = false;
        }
        CHECK(all_false, "real content.db Tradeable=false records read tradeable==false in ContentDb and ItemCatalog");
    }
    // Minimal fixture without a `record` table defaults to tradeable = true
    const fs::path dir = content_fixture::make_temp_dir("worldd_trade_fixture_");
    const fs::path minimal_path = dir / "content.db";
    CHECK(content_fixture::make_db(minimal_path), "minimal content.db fixture created");
    const auto min_db = content::ContentDb::open(minimal_path.string(), err);
    CHECK(min_db.has_value(), "minimal fixture opens");
    if (min_db) {
        const auto rec = min_db->item(content_fixture::item::kPotion, err);
        CHECK(rec.has_value() && rec->tradeable, "minimal fixture without record table defaults tradeable==true");
    }
}
AOC_REGISTER_TEST(trade_content_db_tradeable);

// ============================================================================
//  3. Happy path (2 live PlayerSessions)
// ============================================================================
static void trade_happy_path_two_sessions() {
    std::printf("3. happy path: 2 live sessions request -> accept -> offers -> remove -> cancel + bTradeInProgress\n");
    TradeWorldFixture fx;
    TradeClient alice(*fx.world, make_trader(kAliceId, "Alice", 0, 0, 0, 250000, default_trader_items(100)), fx.t, fx.now_sec(), 0);
    TradeClient bob(*fx.world, make_trader(kBobId, "Bob", 300, 0, 0, 50000, default_trader_items(200)), fx.t, fx.now_sec(), 1);
    fx.settle_join_and_phase_in({&alice, &bob});

    // Request
    alice.send_request_trade(bob.guid(), fx.t);
    fx.settle({&alice, &bob}, 5);
    CHECK(bob.mirror.requests.size() == 1 && bob.mirror.requests[0].character_guid == alice.guid(),
          "Bob receives ClientRequestTrade(Alice)");

    // Accept
    bob.send_trade_response(alice.guid(), static_cast<uint8_t>(repl::TradeRequestResponse::Accept), fx.t);
    fx.settle({&alice, &bob}, 5);
    CHECK(alice.mirror.begins.size() == 1 && bob.mirror.begins.size() == 1, "both receive ClientBeginTrade");
    repl::GuidValue trade_guid{};
    if (!alice.mirror.begins.empty() && !bob.mirror.begins.empty()) {
        trade_guid = alice.mirror.begins[0].trade_request_guid;
        const bool nonzero = (trade_guid.a | trade_guid.b | trade_guid.c | trade_guid.d) != 0;
        CHECK(nonzero && trade_guid == bob.mirror.begins[0].trade_request_guid &&
                  alice.mirror.begins[0].target_guid == bob.guid() && bob.mirror.begins[0].target_guid == alice.guid(),
              "ClientBeginTrade carries identical non-zero TradeGuid and opposite partner GUIDs");
    }
    CHECK(alice.mirror.trade_in_progress && bob.mirror.trade_in_progress, "own pawns replicate bTradeInProgress=true");
    CHECK(fx.observer_trade_in_progress(alice) && fx.observer_trade_in_progress(bob), "observer pawns replicate bTradeInProgress=true");

    // 3 initial zero-copper updates (Version 1..3)
    alice.mirror.clear_events();
    bob.mirror.clear_events();
    for (int32_t v = 1; v <= 3; ++v) {
        repl::TradeOffer empty;
        empty.money = repl::trade_money_container(0);
        empty.version = v;
        alice.send_update_items(trade_guid, empty, fx.t);
        fx.settle({&alice, &bob}, 3);
    }
    CHECK(alice.mirror.updates.size() == 3 && bob.mirror.updates.size() == 3 &&
              bob.mirror.updates.back().offer.version == 3 && bob.mirror.updates.back().character_guid == alice.guid(),
          "3 initial zero-copper updates echo ClientUpdateTrade to both players");

    // Alice offers backpack item + material-bag item + 150000 copper (Version 4)
    alice.mirror.clear_events();
    bob.mirror.clear_events();
    const svc::InventoryItem* a_bag = alice.s->inventory().at(ItemContainer::Backpack, 0);
    const svc::InventoryItem* a_mat = alice.s->inventory().at(ItemContainer::Resource0, 0);
    CHECK(a_bag != nullptr && a_mat != nullptr, "Alice holds backpack and material-bag items");
    if (a_bag && a_mat) {
        repl::TradeOffer offer;
        offer.items.push_back(items::to_instance(*a_bag));
        offer.items.push_back(items::to_instance(*a_mat));
        offer.money = repl::trade_money_container(150000);
        offer.version = 4;
        alice.send_update_items(trade_guid, offer, fx.t);
        fx.settle({&alice, &bob}, 5);
        const bool echoed = alice.mirror.updates.size() == 1 && bob.mirror.updates.size() == 1 &&
                            bob.mirror.updates[0].character_guid == alice.guid() &&
                            bob.mirror.updates[0].offer.items.size() == 2 &&
                            bob.mirror.updates[0].offer.money == repl::trade_money_container(150000) &&
                            bob.mirror.updates[0].offer.version == 4;
        CHECK(echoed, "backpack + material-bag + 150000 copper offer echoes authoritatively to both");
    }

    // Alice removes all items and zeroes copper (Version 5)
    alice.mirror.clear_events();
    bob.mirror.clear_events();
    {
        repl::TradeOffer cleared;
        cleared.money = repl::trade_money_container(0);
        cleared.version = 5;
        alice.send_update_items(trade_guid, cleared, fx.t);
        fx.settle({&alice, &bob}, 5);
        CHECK(alice.mirror.updates.size() == 1 && bob.mirror.updates.size() == 1 &&
                  bob.mirror.updates[0].offer.items.empty() &&
                  bob.mirror.updates[0].offer.money == repl::trade_money_container(0) &&
                  bob.mirror.updates[0].offer.version == 5,
              "removing items and zeroing copper echoes to both");
    }

    // Cancel trade
    alice.mirror.clear_events();
    bob.mirror.clear_events();
    alice.send_transaction(bob.guid(), trade_guid, static_cast<uint8_t>(repl::TradeTransactionRequest::CancelTrade), fx.t);
    fx.settle({&alice, &bob}, 5);
    CHECK(alice.mirror.closes.size() == 1 && alice.mirror.closes[0].status == 0 &&
              bob.mirror.closes.size() == 1 && bob.mirror.closes[0].status == 0,
          "CANCEL_TRADE sends ClientCloseTrade(0) to both");
    CHECK(!alice.mirror.trade_in_progress && !bob.mirror.trade_in_progress, "own pawns clear bTradeInProgress=false");
    CHECK(!fx.observer_trade_in_progress(alice) && !fx.observer_trade_in_progress(bob), "observer pawns clear bTradeInProgress=false");
}
AOC_REGISTER_TEST(trade_happy_path_two_sessions);

// ============================================================================
//  4. Request lifecycle & rejections (decline, timeout, busy, range, status)
// ============================================================================
static void trade_request_lifecycle_and_rejections() {
    std::printf("4. request lifecycle & rejections: decline, 60s expiry, stale accept, self/offline/unphased/busy/range/status\n");
    TradeWorldFixture fx;
    TradeClient alice(*fx.world, make_trader(kAliceId, "Alice", 0, 0, 0, 1000, default_trader_items(100)), fx.t, fx.now_sec(), 0);
    TradeClient bob(*fx.world, make_trader(kBobId, "Bob", 300, 0, 0, 1000, default_trader_items(200)), fx.t, fx.now_sec(), 1);
    TradeClient carol(*fx.world, make_trader(kCarolId, "Carol", 400, 0, 0, 1000, default_trader_items(300)), fx.t, fx.now_sec(), 2);
    // Join all three, phase in Alice and Bob ONLY first (Carol stays unphased)
    fx.settle({&alice, &bob, &carol}, 40);
    alice.phase_in(fx.t);
    bob.phase_in(fx.t);
    fx.settle({&alice, &bob, &carol}, 20);
    alice.mirror.clear_events();
    bob.mirror.clear_events();
    carol.mirror.clear_events();

    // Self-trade -> 5=REJECT
    alice.send_request_trade(alice.guid(), fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.responses.size() == 1 && alice.mirror.responses[0].response == 5, "self-trade rejected with 5=REJECT");

    // Offline target -> 2=PLAYER_NOT_FOUND
    alice.mirror.clear_events();
    const repl::GuidValue offline{0xdeadbeefu, 1u, 2u, 3u};
    alice.send_request_trade(offline, fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.responses.size() == 1 && alice.mirror.responses[0].response == 2, "offline target rejected with 2=PLAYER_NOT_FOUND");

    // Unphased target (Carol) -> 5=REJECT
    alice.mirror.clear_events();
    alice.send_request_trade(carol.guid(), fx.t);
    fx.settle({&alice, &carol}, 4);
    CHECK(alice.mirror.responses.size() == 1 && alice.mirror.responses[0].response == 5, "unphased target rejected with 5=REJECT");

    // Phase in Carol now
    carol.phase_in(fx.t);
    fx.settle({&alice, &bob, &carol}, 20);
    alice.mirror.clear_events();
    bob.mirror.clear_events();
    carol.mirror.clear_events();

    // Decline (1=DECLINE) and busy while pending (4=BUSY)
    alice.send_request_trade(bob.guid(), fx.t);
    fx.settle({&alice, &bob, &carol}, 4);
    CHECK(bob.mirror.requests.size() == 1, "pending request created for Bob");
    carol.send_request_trade(bob.guid(), fx.t);
    fx.settle({&alice, &bob, &carol}, 4);
    CHECK(carol.mirror.responses.size() == 1 && carol.mirror.responses[0].response == 4, "busy target (pending) rejected with 4=BUSY");
    alice.mirror.clear_events();
    bob.send_trade_response(alice.guid(), static_cast<uint8_t>(repl::TradeRequestResponse::Decline), fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.responses.size() == 1 && alice.mirror.responses[0].response == 1, "target decline sends 1=DECLINE to requester");

    // Client timeout auto-response (3=TIMEOUT)
    alice.mirror.clear_events();
    bob.mirror.clear_events();
    alice.send_request_trade(bob.guid(), fx.t);
    fx.settle({&alice, &bob}, 4);
    bob.send_trade_response(alice.guid(), static_cast<uint8_t>(repl::TradeRequestResponse::Timeout), fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.responses.size() == 1 && alice.mirror.responses[0].response == 3, "client timeout sends 3=TIMEOUT to requester");

    // 60 s server expiry + stale accept after expiry
    alice.mirror.clear_events();
    bob.mirror.clear_events();
    alice.send_request_trade(bob.guid(), fx.t);
    fx.settle({&alice, &bob}, 4);
    fx.t += seconds(61);
    fx.step({&alice, &bob}, milliseconds(20));
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.responses.size() == 1 && alice.mirror.responses[0].response == 3, "60 s server expiry sends 3=TIMEOUT to requester");
    alice.mirror.clear_events();
    bob.mirror.clear_events();
    bob.send_trade_response(alice.guid(), static_cast<uint8_t>(repl::TradeRequestResponse::Accept), fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.begins.empty() && bob.mirror.begins.empty(), "stale accept after 60 s expiry is dropped silently");

    // Distance > 10 m at request (including vertical 3D Z > 10 m) -> 6=REJECT_DISTANCE
    bob.s->gm_teleport(bob.conn, 0, 0, 1005.0, fx.t, "test");
    fx.settle({&alice, &bob}, 4);
    alice.mirror.clear_events();
    alice.send_request_trade(bob.guid(), fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.responses.size() == 1 && alice.mirror.responses[0].response == 6, ">10 m 3D distance at request rejected with 6=REJECT_DISTANCE");

    // Walked > 10 m before accept -> 6=REJECT_DISTANCE at accept
    bob.s->gm_teleport(bob.conn, 300, 0, 0, fx.t, "test");
    fx.settle({&alice, &bob}, 4);
    alice.mirror.clear_events();
    bob.mirror.clear_events();
    alice.send_request_trade(bob.guid(), fx.t);
    fx.settle({&alice, &bob}, 4);
    bob.s->gm_teleport(bob.conn, 1500, 0, 0, fx.t, "test");
    fx.settle({&alice, &bob}, 4);
    bob.send_trade_response(alice.guid(), static_cast<uint8_t>(repl::TradeRequestResponse::Accept), fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.begins.empty() && bob.mirror.begins.empty() &&
              alice.mirror.responses.size() == 1 && alice.mirror.responses[0].response == 6,
          "walking >10 m before accept rejects with 6=REJECT_DISTANCE");

    // Move back into range for combat / corrupted / dead tests
    bob.s->gm_teleport(bob.conn, 300, 0, 0, fx.t, "test");
    fx.settle({&alice, &bob}, 4);

    // In-combat at request and at accept -> 5=REJECT
    alice.s->set_combat_flag_for_test(true);
    fx.settle({&alice, &bob}, 4);
    alice.mirror.clear_events();
    alice.send_request_trade(bob.guid(), fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.responses.size() == 1 && alice.mirror.responses[0].response == 5, "in-combat at request rejected with 5=REJECT");
    alice.s->set_combat_flag_for_test(false);
    fx.settle({&alice, &bob}, 4);

    alice.mirror.clear_events();
    bob.mirror.clear_events();
    alice.send_request_trade(bob.guid(), fx.t);
    fx.settle({&alice, &bob}, 4);
    bob.s->set_combat_flag_for_test(true);
    fx.settle({&alice, &bob}, 4);
    bob.send_trade_response(alice.guid(), static_cast<uint8_t>(repl::TradeRequestResponse::Accept), fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.begins.empty() && alice.mirror.responses.size() == 1 && alice.mirror.responses[0].response == 5,
          "in-combat at accept rejected with 5=REJECT");
    bob.s->set_combat_flag_for_test(false);
    fx.settle({&alice, &bob}, 4);

    // Corrupted at request and at accept -> 7=REJECT_CORRUPTED
    bob.s->gm_pvp(bob.conn, "corruption", 100.0, fx.t, "test");
    fx.settle({&alice, &bob}, 4);
    alice.mirror.clear_events();
    alice.send_request_trade(bob.guid(), fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.responses.size() == 1 && alice.mirror.responses[0].response == 7, "corrupted target at request rejected with 7=REJECT_CORRUPTED");
    bob.s->gm_pvp(bob.conn, "corruption", 0.0, fx.t, "test");
    fx.settle({&alice, &bob}, 4);

    alice.mirror.clear_events();
    bob.mirror.clear_events();
    alice.send_request_trade(bob.guid(), fx.t);
    fx.settle({&alice, &bob}, 4);
    bob.s->gm_pvp(bob.conn, "corruption", 50.0, fx.t, "test");
    fx.settle({&alice, &bob}, 4);
    bob.send_trade_response(alice.guid(), static_cast<uint8_t>(repl::TradeRequestResponse::Accept), fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.begins.empty() && alice.mirror.responses.size() == 1 && alice.mirror.responses[0].response == 7,
          "corrupted at accept rejected with 7=REJECT_CORRUPTED");
    bob.s->gm_pvp(bob.conn, "corruption", 0.0, fx.t, "test");
    fx.settle({&alice, &bob}, 4);

    // Dead at request and at accept -> 5=REJECT
    alice.mirror.clear_events();
    bob.mirror.clear_events();
    alice.send_request_trade(bob.guid(), fx.t);
    fx.settle({&alice, &bob}, 4);
    bob.s->take_damage(bob.conn, 1000000.0, fx.t);
    fx.settle({&alice, &bob}, 6);
    bob.send_trade_response(alice.guid(), static_cast<uint8_t>(repl::TradeRequestResponse::Accept), fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.begins.empty() && alice.mirror.responses.size() == 1 && alice.mirror.responses[0].response == 5,
          "dead responder at accept rejected with 5=REJECT");
    alice.mirror.clear_events();
    alice.send_request_trade(bob.guid(), fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.responses.size() == 1 && alice.mirror.responses[0].response == 5, "dead target at request rejected with 5=REJECT");
}
AOC_REGISTER_TEST(trade_request_lifecycle_and_rejections);

// ============================================================================
//  5. Item & currency validation + sender UI revert at incoming.version + 1
// ============================================================================
static void trade_item_and_currency_validation_and_revert() {
    std::printf("5. item & currency validation + authoritative sender UI revert (partner unchanged)\n");
    // Give Alice 14 valid backpack items + untradeable + cargo crate + equipped + quest + resource
    std::vector<svc::InventoryItem> alice_items = default_trader_items(100);
    for (int i = 0; i < 13; ++i) {
        alice_items.push_back(make_item(ItemContainer::Backpack, 10 + i, kRecTradeableStack, 500 + static_cast<uint32_t>(i), 2, 1));
    }

    TradeWorldFixture fx;
    TradeClient alice(*fx.world, make_trader(kAliceId, "Alice", 0, 0, 0, 5000, alice_items), fx.t, fx.now_sec(), 0);
    TradeClient bob(*fx.world, make_trader(kBobId, "Bob", 200, 0, 0, 5000, default_trader_items(200)), fx.t, fx.now_sec(), 1);
    fx.settle_join_and_phase_in({&alice, &bob});

    // Open active trade
    alice.send_request_trade(bob.guid(), fx.t);
    fx.settle({&alice, &bob}, 4);
    bob.send_trade_response(alice.guid(), 0, fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.begins.size() == 1, "(trade active)");
    if (alice.mirror.begins.empty()) return;
    const repl::GuidValue tg = alice.mirror.begins[0].trade_request_guid;

    // Baseline valid offer (Version 1): 1 valid backpack item + 500 copper
    const svc::InventoryItem* base_item = alice.s->inventory().at(ItemContainer::Backpack, 0);
    const repl::ItemInstance base_inst = items::to_instance(*base_item);
    repl::TradeOffer baseline;
    baseline.items.push_back(base_inst);
    baseline.money = repl::trade_money_container(500);
    baseline.version = 1;
    alice.mirror.clear_events();
    bob.mirror.clear_events();
    alice.send_update_items(tg, baseline, fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.updates.size() == 1 && bob.mirror.updates.size() == 1, "baseline valid offer applied");

    int32_t next_v = 2;
    int32_t last_bad_v = 0;
    const auto expect_revert = [&](const char* label, const repl::TradeOffer& bad_in, const repl::TradeOffer& expected_prev) {
        repl::TradeOffer bad = bad_in;
        bad.version = next_v;
        last_bad_v = bad.version;
        next_v += 2; // client receives revert at bad.version + 1, so next client edit is bad.version + 2
        alice.mirror.clear_events();
        bob.mirror.clear_events();
        alice.send_update_items(tg, bad, fx.t);
        fx.settle({&alice, &bob}, 4);
        const bool reverted = alice.mirror.updates.size() == 1 &&
                              alice.mirror.updates[0].character_guid == alice.guid() &&
                              alice.mirror.updates[0].offer.version == bad.version + 1 &&
                              alice.mirror.updates[0].offer.items == expected_prev.items &&
                              alice.mirror.updates[0].offer.money == expected_prev.money &&
                              bob.mirror.updates.empty();
        CHECK(reverted, label);
    };

    // 1. Real content.db Tradeable=false item
    {
        const svc::InventoryItem* untradeable = alice.s->inventory().at(ItemContainer::Backpack, 2);
        CHECK(untradeable != nullptr, "(untradeable item present)");
        if (untradeable) {
            repl::TradeOffer o;
            o.items.push_back(items::to_instance(*untradeable));
            o.money = repl::trade_money_container(0);
            expect_revert("untradeable item (content.db Tradeable=false) rejected & reverts sender UI", o, baseline);
        }
    }
    // 2. Equipped item
    {
        const svc::InventoryItem* equipped = alice.s->inventory().at(ItemContainer::Equipment, items::equip::Helmet);
        CHECK(equipped != nullptr, "(equipped item present)");
        if (equipped) {
            repl::TradeOffer o;
            o.items.push_back(items::to_instance(*equipped));
            o.money = repl::trade_money_container(0);
            expect_revert("equipped item rejected & reverts sender UI", o, baseline);
        }
    }
    // 3. Quest storage item
    {
        const svc::InventoryItem* qitem = alice.s->inventory().at(ItemContainer::Quest, 0);
        CHECK(qitem != nullptr, "(quest item present)");
        if (qitem) {
            repl::TradeOffer o;
            o.items.push_back(items::to_instance(*qitem));
            o.money = repl::trade_money_container(0);
            expect_revert("quest item rejected & reverts sender UI", o, baseline);
        }
    }
    // 4. Cargo crate item (repaired into Resource0 at join; lookup by instance GUID)
    {
        const svc::InventoryItem* crate = alice.s->inventory().find_guid(item_guid_n(113));
        CHECK(crate != nullptr, "(cargo crate present)");
        if (crate) {
            repl::TradeOffer o;
            o.items.push_back(items::to_instance(*crate));
            o.money = repl::trade_money_container(0);
            expect_revert("cargo crate rejected & reverts sender UI", o, baseline);
        }
    }
    // 5. Reserved consumable item (reserve unoffered backpack slot 1 so baseline stays valid)
    {
        const svc::InventoryItem* other_item = alice.s->inventory().at(ItemContainer::Backpack, 1);
        CHECK(other_item != nullptr, "(other backpack item present)");
        if (other_item) {
            alice.s->reserve_item_for_test(9001, other_item->instance_guid);
            repl::TradeOffer o;
            o.items.push_back(items::to_instance(*other_item));
            o.money = repl::trade_money_container(100);
            expect_revert("reserved consumable item rejected & reverts sender UI", o, baseline);
            alice.s->unreserve_item_for_test(9001);
        }
    }
    // 6. Forged quantity
    {
        repl::TradeOffer o;
        repl::ItemInstance forged = base_inst;
        forged.quantity = base_inst.quantity + 1;
        o.items.push_back(forged);
        o.money = repl::trade_money_container(0);
        expect_revert("forged quantity rejected & reverts sender UI", o, baseline);
    }
    // 7. Forged quality
    {
        repl::TradeOffer o;
        repl::ItemInstance forged = base_inst;
        forged.quality = base_inst.quality + 10;
        o.items.push_back(forged);
        o.money = repl::trade_money_container(0);
        expect_revert("forged quality rejected & reverts sender UI", o, baseline);
    }
    // 8. Forged record ID
    {
        repl::TradeOffer o;
        repl::ItemInstance forged = base_inst;
        forged.record_id = kRecTradeableAlt1;
        o.items.push_back(forged);
        o.money = repl::trade_money_container(0);
        expect_revert("forged record ID rejected & reverts sender UI", o, baseline);
    }
    // 9. Duplicate GUID
    {
        repl::TradeOffer o;
        o.items.push_back(base_inst);
        o.items.push_back(base_inst);
        o.money = repl::trade_money_container(0);
        expect_revert("duplicate item GUID rejected & reverts sender UI", o, baseline);
    }
    // 10. Non-Money currency tier & duplicate currency entries
    {
        repl::TradeOffer o;
        o.money.entries.push_back(repl::CurrencyEntry{items::kNodeCurrencyTier, repl::kTradeMoneyTierType, 10});
        expect_revert("non-Money currency tier rejected & reverts sender UI", o, baseline);
    }
    {
        repl::CurrencyContainer dup;
        dup.entries.push_back(repl::CurrencyEntry{items::kMoneyTier, repl::kTradeMoneyTierType, 10});
        dup.entries.push_back(repl::CurrencyEntry{items::kMoneyTier, repl::kTradeMoneyTierType, 10});
        CHECK(!repl::currency_container_fits(dup), "duplicate currency tier rejected at wire codec level");

        repl::TradeOffer o;
        o.money.entries.push_back(repl::CurrencyEntry{items::kMoneyTier, repl::kTradeMoneyTierType, 10});
        o.money.entries.push_back(repl::CurrencyEntry{items::kNodeCurrencyTier, repl::kTradeMoneyTierType, 10});
        expect_revert("multi-entry currency container rejected & reverts sender UI", o, baseline);
    }
    // 11. Negative copper, over-balance copper, over-cap copper
    {
        repl::TradeOffer o;
        o.money = repl::trade_money_container(-1);
        expect_revert("negative copper rejected & reverts sender UI", o, baseline);
    }
    {
        repl::TradeOffer o;
        o.money = repl::trade_money_container(5001);
        expect_revert("over-balance copper rejected & reverts sender UI", o, baseline);
    }
    {
        repl::TradeOffer o;
        o.money = repl::trade_money_container(items::kMoneyCap + 1);
        expect_revert("over-cap copper rejected & reverts sender UI", o, baseline);
    }
    // 12. 12 backpack stacks succeeds, 13th backpack stack rejected & reverts to the 12-stack offer!
    repl::TradeOffer twelve;
    for (int i = 0; i < 12; ++i) {
        twelve.items.push_back(items::to_instance(*alice.s->inventory().at(ItemContainer::Backpack, 10 + i)));
    }
    twelve.money = repl::trade_money_container(200);
    twelve.version = next_v++;
    alice.mirror.clear_events();
    bob.mirror.clear_events();
    alice.send_update_items(tg, twelve, fx.t);
    fx.settle({&alice, &bob}, 5);
    CHECK(alice.mirror.updates.size() == 1 && bob.mirror.updates.size() == 1 &&
              bob.mirror.updates[0].offer.items.size() == 12,
          "12 backpack stacks accepted and echoed to both");
    {
        repl::TradeOffer thirteen = twelve;
        thirteen.items.push_back(items::to_instance(*alice.s->inventory().at(ItemContainer::Backpack, 22)));
        expect_revert("13th backpack stack rejected & reverts to 12-stack offer", thirteen, twelve);
    }
    // 13. >64 total stacks rejected & reverts to 12-stack offer
    {
        repl::TradeOffer over64;
        for (int i = 0; i < 65; ++i) over64.items.push_back(base_inst);
        over64.money = repl::trade_money_container(0);
        expect_revert(">64 total stacks rejected & reverts to 12-stack offer", over64, twelve);
    }
    // 14. Post-revert valid edit at bad.version + 2 succeeds and echoes to both Alice and Bob
    {
        repl::TradeOffer post_revert;
        post_revert.items.push_back(base_inst);
        post_revert.money = repl::trade_money_container(350);
        post_revert.version = last_bad_v + 2;
        alice.mirror.clear_events();
        bob.mirror.clear_events();
        alice.send_update_items(tg, post_revert, fx.t);
        fx.settle({&alice, &bob}, 4);
        CHECK(alice.mirror.updates.size() == 1 && bob.mirror.updates.size() == 1 &&
                  alice.mirror.updates[0].offer.version == last_bad_v + 2 &&
                  bob.mirror.updates[0].offer.version == last_bad_v + 2 &&
                  bob.mirror.updates[0].offer.items == post_revert.items &&
                  bob.mirror.updates[0].offer.money == post_revert.money,
              "post-revert valid edit at bad.version + 2 succeeds and echoes to both");
        next_v = last_bad_v + 3;
    }
    // 15. Pipelined valid edit at bad.version + 1 (sent before client received revert) is still processed
    {
        repl::TradeOffer bad_pipe;
        bad_pipe.money = repl::trade_money_container(-5);
        expect_revert("invalid edit before pipelined edit reverts at V + 1", bad_pipe, [&]() {
            repl::TradeOffer prev;
            prev.items.push_back(base_inst);
            prev.money = repl::trade_money_container(350);
            return prev;
        }());

        repl::TradeOffer pipelined;
        pipelined.items.push_back(base_inst);
        pipelined.money = repl::trade_money_container(400);
        pipelined.version = last_bad_v + 1;
        alice.mirror.clear_events();
        bob.mirror.clear_events();
        alice.send_update_items(tg, pipelined, fx.t);
        fx.settle({&alice, &bob}, 4);
        CHECK(alice.mirror.updates.size() == 1 && bob.mirror.updates.size() == 1 &&
                  alice.mirror.updates[0].offer.version == last_bad_v + 1 &&
                  bob.mirror.updates[0].offer.version == last_bad_v + 1 &&
                  bob.mirror.updates[0].offer.money == pipelined.money,
              "pipelined valid edit at bad.version + 1 is processed cleanly without wedging");
        next_v = last_bad_v + 2;
    }
    // 16. INT32_MAX version (both valid and invalid offers) rejected cleanly without overflow or mutating state
    {
        repl::TradeOffer max_bad;
        max_bad.money = repl::trade_money_container(-1);
        max_bad.version = std::numeric_limits<int32_t>::max();
        repl::TradeOffer max_good;
        max_good.items.push_back(base_inst);
        max_good.money = repl::trade_money_container(450);
        max_good.version = std::numeric_limits<int32_t>::max();
        alice.mirror.clear_events();
        bob.mirror.clear_events();
        alice.send_update_items(tg, max_bad, fx.t);
        alice.send_update_items(tg, max_good, fx.t);
        fx.settle({&alice, &bob}, 4);
        CHECK(alice.mirror.updates.empty() && bob.mirror.updates.empty() &&
                  fx.world->trade_book()->in_trade(alice.guid_words()),
              "INT32_MAX version rejected cleanly without emitting overflowed revert or mutating state");
        CHECK(!fx.world->trade_book()->apply_offer(alice.guid_words(), trade::from_guid_value(tg),
                                                   std::numeric_limits<int32_t>::max(), false, {}, {}, 0) &&
                  !fx.world->trade_book()->apply_offer(alice.guid_words(), trade::from_guid_value(tg),
                                                       std::numeric_limits<int32_t>::max(), true, {}, {}, 0),
              "TradeBook::apply_offer directly rejects INT32_MAX version");

        // Follow-up normal version proves INT32_MAX did not clobber last_version
        repl::TradeOffer after_max;
        after_max.items.push_back(base_inst);
        after_max.money = repl::trade_money_container(425);
        after_max.version = next_v++;
        alice.mirror.clear_events();
        bob.mirror.clear_events();
        alice.send_update_items(tg, after_max, fx.t);
        fx.settle({&alice, &bob}, 4);
        CHECK(alice.mirror.updates.size() == 1 && bob.mirror.updates.size() == 1 &&
                  bob.mirror.updates[0].offer.version == after_max.version &&
                  bob.mirror.updates[0].offer.money == after_max.money,
              "subsequent normal version succeeds after INT32_MAX drop");
    }
}
AOC_REGISTER_TEST(trade_item_and_currency_validation_and_revert);

// ============================================================================
//  6. Monotonicity, wrong TradeGuid isolation, and missing-pawn cancel fallback
// ============================================================================
static void trade_monotonicity_and_isolation() {
    std::printf("6. monotonicity (Version <= last_version), wrong TradeGuid isolation, missing-pawn cancel fallback\n");
    TradeWorldFixture fx;
    TradeClient alice(*fx.world, make_trader(kAliceId, "Alice", 0, 0, 0, 10000, default_trader_items(100)), fx.t, fx.now_sec(), 0);
    TradeClient bob(*fx.world, make_trader(kBobId, "Bob", 200, 0, 0, 10000, default_trader_items(200)), fx.t, fx.now_sec(), 1);
    fx.settle_join_and_phase_in({&alice, &bob});

    alice.send_request_trade(bob.guid(), fx.t);
    fx.settle({&alice, &bob}, 4);
    bob.send_trade_response(alice.guid(), 0, fx.t);
    fx.settle({&alice, &bob}, 4);
    if (alice.mirror.begins.empty()) {
        CHECK(false, "trade failed to begin");
        return;
    }
    const repl::GuidValue tg = alice.mirror.begins[0].trade_request_guid;

    // Valid Version 5
    repl::TradeOffer v5;
    v5.money = repl::trade_money_container(1000);
    v5.version = 5;
    alice.mirror.clear_events();
    bob.mirror.clear_events();
    alice.send_update_items(tg, v5, fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(bob.mirror.updates.size() == 1 && bob.mirror.updates[0].offer.version == 5, "Version 5 applied");

    // Stale Version 5 (equal), Version 4 (older), Version 0 -> all dropped silently
    alice.mirror.clear_events();
    bob.mirror.clear_events();
    for (int32_t stale_v : {5, 4, 0}) {
        repl::TradeOffer stale;
        stale.money = repl::trade_money_container(1);
        stale.version = stale_v;
        alice.send_update_items(tg, stale, fx.t);
    }
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.updates.empty() && bob.mirror.updates.empty(), "Version <= last_version dropped silently");

    // Wrong TradeGuid on ServerUpdateTradeItems and ServerUpdateTradeTransaction -> dropped silently
    const repl::GuidValue wrong_tg{0x12345678u, 0x9abcdef0u, 0x11112222u, 0x33334444u};
    repl::TradeOffer wrong_offer;
    wrong_offer.money = repl::trade_money_container(2);
    wrong_offer.version = 10;
    alice.send_update_items(wrong_tg, wrong_offer, fx.t);
    alice.send_transaction(bob.guid(), wrong_tg, static_cast<uint8_t>(repl::TradeTransactionRequest::CancelTrade), fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.updates.empty() && alice.mirror.closes.empty() && bob.mirror.closes.empty() &&
              fx.world->trade_book()->in_trade(alice.guid_words()),
          "wrong TradeGuid dropped silently without affecting active trade");

    // Missing partner pawn fallback: ServerUpdateTradeTransaction(0, partnerGuid, CANCEL_TRADE)
    alice.send_transaction(repl::GuidValue{}, bob.guid(), static_cast<uint8_t>(repl::TradeTransactionRequest::CancelTrade), fx.t);
    fx.settle({&alice, &bob}, 4);
    CHECK(alice.mirror.closes.size() == 1 && alice.mirror.closes[0].status == 0 &&
              bob.mirror.closes.size() == 1 && bob.mirror.closes[0].status == 0 &&
              !fx.world->trade_book()->in_trade(alice.guid_words()),
          "missing-pawn CANCEL_TRADE(0, partnerGuid, 1) cancels cleanly for both");
}
AOC_REGISTER_TEST(trade_monotonicity_and_isolation);

// ============================================================================
//  7. Mid-trade auto-cancels & relog-safe pawn guard
// ============================================================================
static void trade_mid_trade_auto_cancels_and_relog_guard() {
    std::printf("7. mid-trade auto-cancels (>10m, combat, death, corruption, disconnect, destructor) + relog pawn guard\n");
    const auto open_trade = [](TradeWorldFixture& fx, TradeClient& a, TradeClient& b) -> bool {
        a.mirror.clear_events();
        b.mirror.clear_events();
        a.send_request_trade(b.guid(), fx.t);
        fx.settle({&a, &b}, 4);
        b.send_trade_response(a.guid(), 0, fx.t);
        fx.settle({&a, &b}, 4);
        const bool ok = a.mirror.begins.size() == 1 && b.mirror.begins.size() == 1;
        a.mirror.clear_events();
        b.mirror.clear_events();
        return ok;
    };

    // Move > 10 m mid-trade
    {
        TradeWorldFixture fx;
        TradeClient a(*fx.world, make_trader(kAliceId, "Alice", 0, 0, 0, 1000, default_trader_items(100)), fx.t, fx.now_sec(), 0);
        TradeClient b(*fx.world, make_trader(kBobId, "Bob", 200, 0, 0, 1000, default_trader_items(200)), fx.t, fx.now_sec(), 1);
        fx.settle_join_and_phase_in({&a, &b});
        CHECK(open_trade(fx, a, b), "(move >10m setup)");
        b.s->gm_teleport(b.conn, 1200, 0, 0, fx.t, "test");
        fx.settle({&a, &b}, 6);
        CHECK(a.mirror.closes.size() == 1 && b.mirror.closes.size() == 1 && !a.mirror.trade_in_progress && !b.mirror.trade_in_progress,
              "moving >10 m mid-trade auto-cancels both");
    }
    // Enter combat mid-trade
    {
        TradeWorldFixture fx;
        TradeClient a(*fx.world, make_trader(kAliceId, "Alice", 0, 0, 0, 1000, default_trader_items(100)), fx.t, fx.now_sec(), 0);
        TradeClient b(*fx.world, make_trader(kBobId, "Bob", 200, 0, 0, 1000, default_trader_items(200)), fx.t, fx.now_sec(), 1);
        fx.settle_join_and_phase_in({&a, &b});
        CHECK(open_trade(fx, a, b), "(combat setup)");
        a.s->set_combat_flag_for_test(true);
        fx.settle({&a, &b}, 6);
        CHECK(a.mirror.closes.size() == 1 && b.mirror.closes.size() == 1 && !a.mirror.trade_in_progress && !b.mirror.trade_in_progress,
              "entering combat mid-trade auto-cancels both");
    }
    // Die mid-trade
    {
        TradeWorldFixture fx;
        TradeClient a(*fx.world, make_trader(kAliceId, "Alice", 0, 0, 0, 1000, default_trader_items(100)), fx.t, fx.now_sec(), 0);
        TradeClient b(*fx.world, make_trader(kBobId, "Bob", 200, 0, 0, 1000, default_trader_items(200)), fx.t, fx.now_sec(), 1);
        fx.settle_join_and_phase_in({&a, &b});
        CHECK(open_trade(fx, a, b), "(death setup)");
        a.s->take_damage(a.conn, 1000000.0, fx.t);
        fx.settle({&a, &b}, 6);
        CHECK(a.mirror.closes.size() == 1 && b.mirror.closes.size() == 1 && !b.mirror.trade_in_progress,
              "dying mid-trade auto-cancels both");
    }
    // Become corrupted mid-trade
    {
        TradeWorldFixture fx;
        TradeClient a(*fx.world, make_trader(kAliceId, "Alice", 0, 0, 0, 1000, default_trader_items(100)), fx.t, fx.now_sec(), 0);
        TradeClient b(*fx.world, make_trader(kBobId, "Bob", 200, 0, 0, 1000, default_trader_items(200)), fx.t, fx.now_sec(), 1);
        fx.settle_join_and_phase_in({&a, &b});
        CHECK(open_trade(fx, a, b), "(corruption setup)");
        b.s->gm_pvp(b.conn, "corruption", 100.0, fx.t, "test");
        fx.settle({&a, &b}, 6);
        CHECK(a.mirror.closes.size() == 1 && b.mirror.closes.size() == 1 && !a.mirror.trade_in_progress && !b.mirror.trade_in_progress,
              "becoming corrupted mid-trade auto-cancels both");
    }
    // Disconnect (finish) & destructor (~PlayerSession) mid-trade
    {
        TradeWorldFixture fx;
        auto a = std::make_unique<TradeClient>(*fx.world, make_trader(kAliceId, "Alice", 0, 0, 0, 1000, default_trader_items(100)), fx.t, fx.now_sec(), 0);
        TradeClient b(*fx.world, make_trader(kBobId, "Bob", 200, 0, 0, 1000, default_trader_items(200)), fx.t, fx.now_sec(), 1);
        fx.settle_join_and_phase_in({a.get(), &b});
        CHECK(open_trade(fx, *a, b), "(finish setup)");
        a->s->finish("disconnect");
        fx.settle({&b}, 6);
        CHECK(b.mirror.closes.size() == 1 && !b.mirror.trade_in_progress && !fx.observer_trade_in_progress(b),
              "disconnect (finish) mid-trade cancels cleanly for partner");

        // Rejoin Alice, start trade, drop ~PlayerSession without finish()
        auto a2 = std::make_unique<TradeClient>(*fx.world, make_trader(kAliceId, "Alice", 0, 0, 0, 1000, default_trader_items(100)), fx.t, fx.now_sec(), 3);
        fx.settle({a2.get(), &b}, 40);
        a2->phase_in(fx.t);
        fx.settle({a2.get(), &b}, 20);
        CHECK(open_trade(fx, *a2, b), "(destructor setup)");
        a2.reset();
        fx.settle({&b}, 6);
        CHECK(b.mirror.closes.size() == 1 && !b.mirror.trade_in_progress,
              "destructor (~PlayerSession without finish) cancels cleanly for partner");
    }
    // Relog-safe pawn guard: old session's late finish()/destructor never cancels relogged session's new trade
    {
        TradeWorldFixture fx;
        auto a_old = std::make_unique<TradeClient>(*fx.world, make_trader(kAliceId, "Alice", 0, 0, 0, 1000, default_trader_items(100)), fx.t, fx.now_sec(), 0);
        TradeClient b(*fx.world, make_trader(kBobId, "Bob", 200, 0, 0, 1000, default_trader_items(200)), fx.t, fx.now_sec(), 1);
        fx.settle_join_and_phase_in({a_old.get(), &b});
        // Alice relogs before a_old is destroyed
        auto a_new = std::make_unique<TradeClient>(*fx.world, make_trader(kAliceId, "Alice", 0, 0, 0, 1000, default_trader_items(100)), fx.t, fx.now_sec(), 4);
        fx.settle({a_new.get(), &b}, 40);
        a_new->phase_in(fx.t);
        fx.settle({a_new.get(), &b}, 20);
        CHECK(open_trade(fx, *a_new, b), "(relogged session opens new trade)");
        // Now old session finishes and destroys!
        a_old->s->finish("replaced by relog");
        a_old.reset();
        fx.settle({a_new.get(), &b}, 6);
        CHECK(a_new->mirror.closes.empty() && b.mirror.closes.empty() &&
                  fx.world->trade_book()->in_trade(a_new->guid_words()) &&
                  fx.world->trade_book()->in_trade(b.guid_words()),
              "old session's late finish/destructor never cancels relogged session's active trade");
    }
}
AOC_REGISTER_TEST(trade_mid_trade_auto_cancels_and_relog_guard);

// ============================================================================
//  8. Mid-trade inventory & wallet drift auto-cancel
// ============================================================================
static void trade_mid_trade_inventory_and_wallet_drift() {
    std::printf("8. mid-trade inventory & wallet drift: equip offered item, split offered stack, spend offered copper\n");
    const auto open_trade = [](TradeWorldFixture& fx, TradeClient& a, TradeClient& b) -> std::optional<repl::GuidValue> {
        a.mirror.clear_events();
        b.mirror.clear_events();
        a.send_request_trade(b.guid(), fx.t);
        fx.settle({&a, &b}, 4);
        b.send_trade_response(a.guid(), 0, fx.t);
        fx.settle({&a, &b}, 4);
        if (a.mirror.begins.empty()) return std::nullopt;
        const repl::GuidValue tg = a.mirror.begins[0].trade_request_guid;
        a.mirror.clear_events();
        b.mirror.clear_events();
        return tg;
    };

    // 8a. Equip offered backpack helmet mid-trade via ServerItemUsed -> auto-cancel
    {
        TradeWorldFixture fx;
        TradeClient a(*fx.world, make_trader(kAliceId, "Alice", 0, 0, 0, 5000, default_trader_items(100)), fx.t, fx.now_sec(), 0);
        TradeClient b(*fx.world, make_trader(kBobId, "Bob", 200, 0, 0, 5000, default_trader_items(200)), fx.t, fx.now_sec(), 1);
        fx.settle_join_and_phase_in({&a, &b});
        const auto tg = open_trade(fx, a, b);
        CHECK(tg.has_value(), "(equip drift setup)");
        if (tg) {
            const svc::InventoryItem* helmet = a.s->inventory().at(ItemContainer::Backpack, 1);
            repl::TradeOffer o;
            o.items.push_back(items::to_instance(*helmet));
            o.money = repl::trade_money_container(0);
            o.version = 1;
            a.send_update_items(*tg, o, fx.t);
            fx.settle({&a, &b}, 4);
            a.mirror.clear_events();
            b.mirror.clear_events();

            // Send ServerItemUsed to equip Backpack slot 1
            const schema::RpcSchema* use_rpc = class_named("PlayerPawn_C").rpc("ServerItemUsed");
            repl::RpcParams p;
            const auto g = *items::guid_of(helmet->instance_guid);
            repl::set_rpc_param(*use_rpc, p, "Storage", repl::Value::u64(static_cast<uint64_t>(repl::ItemLocation::Backpack)));
            repl::set_rpc_param(*use_rpc, p, "Slot", repl::Value::i64(1));
            repl::set_rpc_param(*use_rpc, p, "ExpectedItem.A", repl::Value::i64(static_cast<int32_t>(g.a)));
            repl::set_rpc_param(*use_rpc, p, "ExpectedItem.B", repl::Value::i64(static_cast<int32_t>(g.b)));
            repl::set_rpc_param(*use_rpc, p, "ExpectedItem.C", repl::Value::i64(static_cast<int32_t>(g.c)));
            repl::set_rpc_param(*use_rpc, p, "ExpectedItem.D", repl::Value::i64(static_cast<int32_t>(g.d)));
            repl::set_rpc_param(*use_rpc, p, "RequestedEquipSlot", repl::Value::i64(static_cast<int32_t>(repl::kAnyEquipSlot)));
            a.send_pawn("ServerItemUsed", p, fx.t);
            fx.settle({&a, &b}, 6);
            CHECK(a.mirror.closes.size() == 1 && b.mirror.closes.size() == 1 && !a.mirror.trade_in_progress && !b.mirror.trade_in_progress,
                  "equipping offered backpack item mid-trade auto-cancels for both");
        }
    }

    // 8b. Split offered backpack stack mid-trade via ServerRequestStorageEvent -> quantity drifts -> auto-cancel
    {
        TradeWorldFixture fx;
        TradeClient a(*fx.world, make_trader(kAliceId, "Alice", 0, 0, 0, 5000, default_trader_items(100)), fx.t, fx.now_sec(), 0);
        TradeClient b(*fx.world, make_trader(kBobId, "Bob", 200, 0, 0, 5000, default_trader_items(200)), fx.t, fx.now_sec(), 1);
        fx.settle_join_and_phase_in({&a, &b});
        const auto tg = open_trade(fx, a, b);
        CHECK(tg.has_value(), "(stack split drift setup)");
        if (tg) {
            const svc::InventoryItem* stack = a.s->inventory().at(ItemContainer::Backpack, 0);
            repl::TradeOffer o;
            o.items.push_back(items::to_instance(*stack));
            o.money = repl::trade_money_container(0);
            o.version = 1;
            a.send_update_items(*tg, o, fx.t);
            fx.settle({&a, &b}, 4);
            a.mirror.clear_events();
            b.mirror.clear_events();

            // Move 2 of 5 from Backpack slot 0 to empty Backpack slot 10 (splits stack -> qty becomes 3)
            const schema::RpcSchema* mv_rpc = class_named("AoCPlayerControllerBP_C").rpc("ServerRequestStorageEvent");
            repl::ItemStorageEvent ev;
            ev.type = static_cast<uint8_t>(repl::StorageEventType::Move);
            ev.move_source.object = a.s->bootstrap().setup().guids.backpack;
            ev.move_source.slots.push_back(repl::StorageEventSlot{0, 2, *items::guid_of(stack->instance_guid)});
            repl::StorageEventContainer dst;
            dst.object = a.s->bootstrap().setup().guids.backpack;
            dst.slots.push_back(repl::StorageEventSlot{10, 0, {}});
            ev.targets.push_back(std::move(dst));
            ev.item = items::to_instance(*stack);
            const auto mp = repl::storage_event_params(*mv_rpc, ev);
            CHECK(mp.has_value(), "(storage split request encoded)");
            if (mp) {
                a.send_pc("ServerRequestStorageEvent", *mp, fx.t);
                fx.settle({&a, &b}, 6);
                CHECK(a.mirror.closes.size() == 1 && b.mirror.closes.size() == 1,
                      "splitting/changing offered stack quantity mid-trade auto-cancels for both");
            }
        }
    }

    // 8c. Spending copper below offered copper mid-trade -> auto-cancel
    {
        TradeWorldFixture fx;
        TradeClient a(*fx.world, make_trader(kAliceId, "Alice", 0, 0, 0, 5000, default_trader_items(100)), fx.t, fx.now_sec(), 0);
        TradeClient b(*fx.world, make_trader(kBobId, "Bob", 200, 0, 0, 5000, default_trader_items(200)), fx.t, fx.now_sec(), 1);
        fx.settle_join_and_phase_in({&a, &b});
        const auto tg = open_trade(fx, a, b);
        CHECK(tg.has_value(), "(wallet drift setup)");
        if (tg) {
            repl::TradeOffer o;
            o.money = repl::trade_money_container(4000);
            o.version = 1;
            a.send_update_items(*tg, o, fx.t);
            fx.settle({&a, &b}, 4);
            a.mirror.clear_events();
            b.mirror.clear_events();

            CHECK(a.s->spend_copper_for_test(2000), "(Alice spends 2000 copper, leaving 3000 < 4000 offered)");
            fx.settle({&a, &b}, 6);
            CHECK(a.mirror.closes.size() == 1 && b.mirror.closes.size() == 1 && !a.mirror.trade_in_progress && !b.mirror.trade_in_progress,
                  "spending copper below offered copper mid-trade auto-cancels for both");
        }
    }
}
AOC_REGISTER_TEST(trade_mid_trade_inventory_and_wallet_drift);

// ============================================================================
//  9. Lock/swap stub boundary (LOCK/UNLOCK/FINALIZE) + RequestBudget flood
// ============================================================================
static void trade_lock_stub_boundary_and_flood() {
    std::printf("9. lock/swap stub boundary (LOCK/UNLOCK/FINALIZE, zero DB writes) + RequestBudget flood\n");
    TradeWorldFixture fx;
    TradeClient a(*fx.world, make_trader(kAliceId, "Alice", 0, 0, 0, 12345, default_trader_items(100)), fx.t, fx.now_sec(), 0);
    TradeClient b(*fx.world, make_trader(kBobId, "Bob", 200, 0, 0, 67890, default_trader_items(200)), fx.t, fx.now_sec(), 1);
    fx.settle_join_and_phase_in({&a, &b});

    const auto a_inv_before = a.s->inventory().items();
    const auto b_inv_before = b.s->inventory().items();
    const int64_t a_copper_before = a.s->wallet().copper();
    const int64_t b_copper_before = b.s->wallet().copper();

    a.send_request_trade(b.guid(), fx.t);
    fx.settle({&a, &b}, 4);
    b.send_trade_response(a.guid(), 0, fx.t);
    fx.settle({&a, &b}, 4);
    if (a.mirror.begins.empty()) {
        CHECK(false, "trade failed to begin");
        return;
    }
    const repl::GuidValue tg = a.mirror.begins[0].trade_request_guid;

    // Both offer items and copper
    {
        repl::TradeOffer oa;
        oa.items.push_back(items::to_instance(*a.s->inventory().at(ItemContainer::Backpack, 0)));
        oa.money = repl::trade_money_container(5000);
        oa.version = 1;
        a.send_update_items(tg, oa, fx.t);

        repl::TradeOffer ob;
        ob.items.push_back(items::to_instance(*b.s->inventory().at(ItemContainer::Backpack, 0)));
        ob.money = repl::trade_money_container(10000);
        ob.version = 1;
        b.send_update_items(tg, ob, fx.t);
        fx.settle({&a, &b}, 5);
    }

    // Send LOCK_TRADE (2), UNLOCK_TRADE (3), FINALIZE_TRADE (4)
    a.mirror.clear_events();
    b.mirror.clear_events();
    for (uint8_t req : {uint8_t{2}, uint8_t{3}, uint8_t{4}}) {
        a.send_transaction(b.guid(), tg, req, fx.t);
    }
    fx.settle({&a, &b}, 6);
    const bool got_callout = std::any_of(a.mirror.callouts.begin(), a.mirror.callouts.end(),
                                         [](const std::string& s) { return s == "Trade lock and swap are not enabled yet."; });
    CHECK(got_callout, "LOCK/UNLOCK/FINALIZE emits lock-disabled callout");
    CHECK(a.mirror.closes.empty() && b.mirror.closes.empty() && fx.world->trade_book()->in_trade(a.guid_words()),
          "LOCK/UNLOCK/FINALIZE does not close or mutate trade state");
    CHECK(a.s->inventory().items() == a_inv_before && b.s->inventory().items() == b_inv_before &&
              a.s->wallet().copper() == a_copper_before && b.s->wallet().copper() == b_copper_before,
          "LOCK/UNLOCK/FINALIZE leaves both inventories and wallets untouched");
    CHECK(fx.sink.inventories.empty(), "zero save_inventory DB writes across trade window & LOCK/UNLOCK/FINALIZE");

    // Flood test: send 30 ServerUpdateTradeItems at the same timestamp -> burst 16 processed, 14 dropped cleanly
    const size_t dropped_before = a.s->requests_dropped();
    const size_t calls_before = a.s->trade_calls();
    // Advance clock 2 s so budget refills to full burst of 16
    fx.t += seconds(2);
    std::vector<fake::Bunch> flood;
    const schema::RpcSchema* upd_rpc = class_named("AoCPlayerControllerBP_C").rpc("ServerUpdateTradeItems");
    for (int i = 0; i < 30; ++i) {
        repl::TradeOffer o;
        o.money = repl::trade_money_container(10 + i);
        o.version = 10 + i;
        const auto p = repl::server_update_trade_items_params(*upd_rpc, tg, o);
        const BunchWriter w = pc_rpc_bunch("ServerUpdateTradeItems", *p);
        flood.push_back(a.client.actor(a.pc_ch, w.data(), w.bit_pos(), true));
    }
    a.deliver(std::move(flood), fx.t);
    fx.settle({&a, &b}, 20);
    CHECK(a.s->trade_calls() - calls_before == 16 && a.s->requests_dropped() - dropped_before == 14 &&
              !a.s->failed() && fx.world->trade_book()->in_trade(a.guid_words()),
          "30-call trade flood capped at burst 16 (14 dropped) without failing session or corrupting trade");
}
AOC_REGISTER_TEST(trade_lock_stub_boundary_and_flood);

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log);
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("test_worldd_trade", sink));
    spdlog::set_level(spdlog::level::info);
    const int extra = aoc_tests::run();
    g_fail += extra;
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
