// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/area_session_market_tests.inc: the Marketplace Manager through a real session
//  (registered with AOC_REGISTER_TEST: test_worldd_area_session.cpp runs them through aoc_tests::run; included from area_session_registered.inc)
//
//    the window        the interact key on the Manager is answered with one ClientOpenMarketplaceUI, with the market's stable guid and the node's
//                      default tax table, then one empty last storefront page; no seller data is sent
//    the vendor        the interact key on a GM vendor still opens the vendor window
//    the reach         a Manager 40 m and one 600 m away open nothing
//    the open          the Manager's open reads to its last bit with its class; his interaction is the marketplace one
//    the requests      a headers request answers the storefront page and nothing else; a data request answers one seller's data and no page
//    the access        no window opened, another market and a Manager 20 m away are all ignored
//    the second login  the same market guid and table
//    two players       both players at one Manager get their own one open
// ============================================================================
#include "items/wallet.h"
#include "market/market.h"
#include "protocol/repl/rpc_fields.h"
#include "spawns/area_loader.h"

namespace market_test {

// The Manager's PopulationInstance (Node_Vendor_Gear_marketplace), its InteractableComponentDef (ICD_ID_MarketplaceVendor)
// and its PlayerInteractionDef (ID_MarketplaceVendor).
inline constexpr int64_t kManagerInstance = 6064632772964253696ll;
inline constexpr int64_t kManagerComponent = 6064633962143220653ll;
inline constexpr int64_t kManagerInteraction = 6064632772967071745ll;
// Joeva, the node the test Manager stands in: 0x62D025765679.
inline constexpr int64_t kNode = 108646121231993ll;
inline constexpr uint32_t kGuidA = 0x25765679u, kGuidB = 0x000062D0u, kGuidC = 0x4D4B5450u, kGuidD = 0x00000001u;
// The GM vendor of the vendor window test.
inline constexpr int64_t kGmGear = 6064634098852102156ll;
// The start purse a character with no wallet is given: 21 gold (100 copper a silver, 100 silver a gold).
inline constexpr int64_t kStartPurse = 21 * 100 * 100;

}  // namespace market_test

/// `c` with the start purse when the character service would give it one: a listing charges its fee, so the seller needs money.
static svc::Character with_start_purse(svc::Character c) {
    if (c.wallet.empty()) c.wallet.push_back({items::kCopperCurrency, market_test::kStartPurse});
    return c;
}

using namespace market_test;

/// The ObjectId of the actor's interactable component (0: none).
static uint64_t market_component(const world::ActorRegistry& registry, uint64_t actor) {
    const world::WorldActor* a = registry.find(actor);
    return a ? a->npc.interact_component : 0;
}

/// The Manager as the node's data makes him: the General_Gear_Base_Vendor open and the Manager instance.  The two ids are
/// the instance's (the recorded open's are the vendor's).
static uint64_t add_market_manager(Fixture& fx, double x, double y, const char* name, int64_t node = kNode) {
    const spawns::OpenTemplate* donor = fx.captured->template_for(spawns::kGmVendorArchetype);
    if (!donor) {
        std::printf("  [FAIL] no General_Gear_Base_Vendor template\n");
        std::exit(2);
    }
    content::NpcAssetRow asset;
    asset.guid = 6064630270761766453ll;
    asset.name = "Node_Vendor_Gear";
    asset.archetype_path = spawns::kGmVendorArchetype;
    content::NpcInstanceRow instance;
    instance.guid = kManagerInstance;
    instance.interact_component = kManagerComponent;
    instance.interact_response = kManagerInteraction;
    instance.level_min = instance.level_max = 25;
    spawns::DataSpawn spawn;
    spawn.x = x;
    spawn.y = y;
    spawn.z = bootstrap::kDefaultSpawn.z;
    spawn.yaw = 0;
    spawn.level = 25;
    spawn.asset = &asset;
    spawn.instance = &instance;
    spawn.open = donor;
    spawn.seed = 0x4D41524B4554ull;
    std::string error;
    spawns::InstanceParams params;
    auto w = spawns::make_data_actor(spawn, fx.minter(), kServerId, 0.0, &params, error);
    if (!w) {
        std::printf("  [FAIL] the Manager is not made: %s\n", error.c_str());
        std::exit(2);
    }
    if (name) w->npc.name = name;
    for (auto& sub : w->actor.subobjects) {
        if (sub.obj.schema && std::string(sub.obj.schema->name) == "UAoCNodePlotComponent") sub.obj.state.set(*sub.obj.schema, "NodeRecordGuid", repl::Value::i64(node));
    }
    world::allow_inputs(w->actor);   // the live world's hook does this for a shopkeeper; the test Manager has no shop link
    const uint64_t id = w->id;
    if (!fx.world->registry().add(std::move(*w))) {
        std::printf("  [FAIL] the registry refuses the Manager\n");
        std::exit(2);
    }
    return id;
}

struct MarketHarness : Harness {
    uint64_t manager = 0, edge = 0, far = 0, unseen = 0, vendor = 0;
    explicit MarketHarness(svc::Character r = cyb_record()) : Harness(with_start_purse(std::move(r))) {
        populate();
        start();
    }
    /// A world that gets `market` as its sink before the session joins: the saved listings are read at that point, as a restarted world does.
    MarketHarness(svc::Character r, PositionSink* market) : Harness(with_start_purse(std::move(r))) {
        fx.world->set_position_sink(market);
        populate();
        start();
    }
    void populate() {
        manager = add_market_manager(fx, kCx + 800, kCy, "Marketplace Manager");
        edge = add_market_manager(fx, kCx + 2000, kCy, "Marketplace Manager Edge");   // inside the interaction range, past the market's 10 m
        far = add_market_manager(fx, kCx + 4000, kCy, "Marketplace Manager Far");
        unseen = add_market_manager(fx, kCx, kCy - 60000, "Marketplace Manager Unseen");
        fx.world->set_gm_vendors(test_vendors());
        const spawns::OpenTemplate* vendor_t = fx.captured->template_for(spawns::kGmVendorArchetype);
        if (!vendor_t) {
            std::printf("  [FAIL] no vendor template\n");
            std::exit(2);
        }
        vendor = fx.add(*vendor_t, kCx, kCy + 900, true, kGmGear, "GM Gear 0-9");
    }
    void talk_to(uint64_t actor) { client_request(interact(market_component(fx.world->registry(), actor)), pawn_ch); }
    /// The interact key on the Manager: the session remembers his market for the calls that follow.
    void open_market(uint64_t actor) { talk_to(actor); }
};

/// A second session on the same world (a second player at the same Manager).
struct MarketPeer {
    MarketHarness&                 host;
    Connection                     conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client                   client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<PlayerSession> s;
    std::vector<fake::Delivered>   got;
    uint32_t                       pawn_ch = 0, pc_ch = 0;
    std::string                    error;
    explicit MarketPeer(MarketHarness& h, svc::Character r) : host(h) {
        s = PlayerSession::join(conn, *host.fx.world, std::move(r), identity(), world_time_at(host.t), error);
        if (!s) {
            std::printf("  [FAIL] the second session does not join: %s\n", error.c_str());
            std::exit(2);
        }
        pawn_ch = s->channel_of(bs::Role::Pawn);
        pc_ch = s->channel_of(bs::Role::PlayerController);
    }
    void step() {
        host.t += milliseconds(20);
        s->tick(conn, host.t);
        for (auto& pkt : conn.flush(host.t)) {
            const auto r = client.receive(pkt);
            for (const auto& d : r.delivered) got.push_back(d);
        }
        s->take_acks(conn);
        client_sends({});
    }
    void client_sends(std::vector<fake::Bunch> bunches) {
        for (auto& pkt : client.packets(std::move(bunches))) {
            const std::vector<InBunch> in = conn.receive(pkt, host.t);
            s->take_acks(conn);
            for (const InBunch& b : in) s->on_bunch(conn, b, host.t);
            s->end_of_datagram(conn);
        }
    }
    void settle() {
        for (int quiet = 0, i = 0; quiet < 5 && i < 600; ++i) {
            const size_t before = got.size();
            step();
            quiet = (got.size() == before && s->join_pending() == 0 && s->replies_pending() == 0) ? quiet + 1 : 0;
        }
    }
    void play() {
        settle();
        client_sends({client.actor(pawn_ch, actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});
        settle();
        for (int i = 0; i < 75; ++i) step();
        settle();
    }
    void request(const BunchWriter& rpc, uint32_t channel) {
        client_sends({client.actor(channel, rpc, true)});
        settle();
    }
};

/// The PlayerController's decoded calls among `got[from..]` of a session (the peer's copy of pc_calls_since).
static std::vector<repl::RpcCall> market_calls_since(const std::vector<fake::Delivered>& got, uint32_t pc_ch, size_t from) {
    std::vector<repl::RpcCall> out;
    for (size_t i = from; i < got.size(); ++i) {
        if (got[i].channel != pc_ch || got[i].open || got[i].close) continue;
        const auto blocks = repl::read_content_blocks(got[i].content.data(), got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            if (!b.header.is_actor || b.header.has_rep_layout) continue;
            size_t p = b.payload_pos;
            auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), got[i].content.data(), b.payload_pos + b.payload_bits, p);
            if (calls) {
                for (auto& c : *calls) out.push_back(std::move(c));
            }
        }
    }
    return out;
}

/// The one ClientOpenMarketplaceUI among `calls` (nullptr: none).
static const repl::RpcCall* market_open_call(const std::vector<repl::RpcCall>& calls) {
    for (const repl::RpcCall& c : calls) {
        if (c.rpc && std::string(c.rpc->name) == "ClientOpenMarketplaceUI") return &c;
    }
    return nullptr;
}

/// The window's open is followed by the storefront page of a market nobody else sells at (one empty page, the last) and sends no seller data (the data answer follows the
/// window's own request).
static void check_no_market_data(const std::vector<repl::RpcCall>& calls) {
    CHECK(count_calls(calls, "ClientReceiveMarketplaceHeaders") == 1 && count_calls(calls, "ClientReceiveMarketplaceData") == 0, "the open sends one storefront page and no seller data: the window has no listing yet");
    for (const repl::RpcCall& c : calls) {
        if (!c.rpc || std::string(c.rpc->name) != "ClientReceiveMarketplaceHeaders") continue;
        const auto* complete = repl::rpcf::as<bool>(repl::rpcf::value_of(*c.rpc, c.params, "bComplete"));
        CHECK(complete && *complete && repl::rpcf::value_of(*c.rpc, c.params, "Headers") == nullptr, "the page is the last one and holds no storefront");
    }
}

/// The one open call's guid words and the node's whole default tax table.
static void check_market_open(const repl::RpcCall& c) {
    CHECK(c.decoded, "the open call reads with the client's schema");
    const auto word = [&](const char* path) { return repl::rpcf::int_or(repl::rpcf::value_of(*c.rpc, c.params, path), 0); };
    const auto a = word("MarketplaceGuid.A"), b = word("MarketplaceGuid.B"), cc = word("MarketplaceGuid.C"), d = word("MarketplaceGuid.D");
    const auto* tax = repl::rpcf::as<repl::NodeTaxDetails>(repl::rpcf::value_of(*c.rpc, c.params, "NodeTaxDetails"));
    const repl::NodeTaxDetails expected = vendor::default_table(kNode);
    bool same = a && b && cc && d && *a == static_cast<int64_t>(kGuidA) && *b == static_cast<int64_t>(kGuidB) && *cc == static_cast<int64_t>(kGuidC) && *d == static_cast<int64_t>(kGuidD);
    CHECK(same, "the market guid is the node's stable one (0x25765679, 0x000062D0, 0x4D4B5450, 1)");
    bool table = tax && tax->node == kNode && tax->entries.size() == expected.entries.size();
    for (size_t i = 0; table && i < tax->entries.size(); ++i) {
        const repl::NodeTaxEntry& x = tax->entries[i];
        const repl::NodeTaxEntry& y = expected.entries[i];
        table = x.tag == y.tag && x.details == y.details && x.flat_tax == y.flat_tax && x.tax_percent == y.tax_percent && x.owning_node_percent == y.owning_node_percent && x.flag_a == y.flag_a && x.flag_b == y.flag_b;
    }
    CHECK(table, "the node's whole default tax table is the one the vendor window sends");
}

/// The Manager's open chain: every block reads to its last bit with its class.
static bool manager_open_reads(const Harness& h, uint64_t id) {
    const world::WorldActor* a = h.fx.world->registry().find(id);
    const auto chains = chains_of(h, id);
    const fake::Delivered* open = nullptr;
    for (const fake::Delivered* d : chains) {
        if (d->open) {
            open = d;
            break;
        }
    }
    if (!a || !open) return false;
    const size_t end = open->content.bit_pos();
    std::optional<std::vector<repl::BlockView>> blocks;
    for (size_t start = 0; start < 2000 && start < end; ++start) {
        auto got = repl::read_content_blocks(open->content.data(), end, start);
        if (!got || got->empty() || !(*got)[0].header.is_actor) continue;
        if (got->size() != a->actor.subobjects.size() + 1) continue;
        blocks = std::move(got);
        break;
    }
    if (!blocks) return false;
    for (const repl::BlockView& b : *blocks) {
        const repl::ObjectState* obj = b.header.is_actor ? &a->actor.obj : nullptr;
        if (!obj) {
            for (const repl::Subobject& s : a->actor.subobjects) {
                if (s.guid.ObjectId == b.header.object.ObjectId) obj = &s.obj;
            }
        }
        if (!obj || !obj->schema) return false;
        const size_t block_end = b.payload_pos + b.payload_bits;
        size_t pos = b.payload_pos;
        if (b.header.has_rep_layout && !repl::read_rep_block(*obj->schema, open->content.data(), block_end, pos, obj->opts)) return false;
        if (pos < block_end && !repl::read_fast_array_fields(*obj->schema, open->content.data(), block_end, pos, obj->opts)) return false;
        if (pos != block_end) return false;
    }
    return true;
}

/// ServerRequestMarketplaceData as the client sends it after the window opens: the market and the seller it asks about (the Sell tab sends his own guid).
static BunchWriter market_data_request(const repl::GuidValue& market, const repl::GuidValue& seller, bool with_seller) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerRequestMarketplaceData");
    if (!rpc) return BunchWriter();
    repl::RpcParams params;
    const auto put = [&](const char* prefix, const repl::GuidValue& g) {
        const std::string p(prefix);
        return repl::set_rpc_param(*rpc, params, p + ".A", repl::Value::i64(static_cast<int32_t>(g.a))) &&
               repl::set_rpc_param(*rpc, params, p + ".B", repl::Value::i64(static_cast<int32_t>(g.b))) &&
               repl::set_rpc_param(*rpc, params, p + ".C", repl::Value::i64(static_cast<int32_t>(g.c))) &&
               repl::set_rpc_param(*rpc, params, p + ".D", repl::Value::i64(static_cast<int32_t>(g.d)));
    };
    const bool ok = put("MarketplaceActorGuid", market) && (!with_seller || put("PlayerGuid", seller));
    return ok ? actor_rpc(pc, "ServerRequestMarketplaceData", params) : BunchWriter();
}

/// ServerRequestMarketplaceHeaders as the window sends it: the market and the version of the headers the client holds.
static BunchWriter market_headers_request(const repl::GuidValue& market, uint64_t version) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerRequestMarketplaceHeaders");
    if (!rpc) return BunchWriter();
    repl::RpcParams params;
    const auto put = [&](const char* prefix, const repl::GuidValue& g) {
        const std::string p(prefix);
        return repl::set_rpc_param(*rpc, params, p + ".A", repl::Value::i64(static_cast<int32_t>(g.a))) &&
               repl::set_rpc_param(*rpc, params, p + ".B", repl::Value::i64(static_cast<int32_t>(g.b))) &&
               repl::set_rpc_param(*rpc, params, p + ".C", repl::Value::i64(static_cast<int32_t>(g.c))) &&
               repl::set_rpc_param(*rpc, params, p + ".D", repl::Value::i64(static_cast<int32_t>(g.d)));
    };
    const bool ok = put("MarketplaceActorGuid", market) && repl::set_rpc_param(*rpc, params, "HeaderVersion", repl::Value::u64(version));
    return ok ? actor_rpc(pc, "ServerRequestMarketplaceHeaders", params) : BunchWriter();
}

static void test_marketplace_window() {
    std::printf("the Marketplace Manager: the interact key opens his window\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    const world::WorldActor* manager = h.fx.world->registry().find(h.manager);
    CHECK(manager != nullptr && manager->npc.interact_def == kManagerInteraction, "the Manager's open carries ID_MarketplaceVendor");
    CHECK(h.s->actor_view()->state_of(h.manager) == world::ClientState::Awake && h.s->actor_view()->state_of(h.far) == world::ClientState::Awake &&
              h.s->actor_view()->state_of(h.unseen) == world::ClientState::None,
          "the Manager at 8 m and 40 m is open on the client, the one at 600 m is not");
    size_t from = h.got.size();
    h.talk_to(h.manager);
    const auto calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientOpenMarketplaceUI") == 1, "the interact key on the Manager sends one ClientOpenMarketplaceUI");
    if (const repl::RpcCall* open = market_open_call(calls)) {
        check_market_open(*open);
    } else {
        CHECK(false, "the open call is there to read");
    }
    check_no_market_data(calls);

    // the same click twice: each one is answered with one window
    from = h.got.size();
    h.talk_to(h.manager);
    CHECK(count_calls(pc_calls_since(h, from), "ClientOpenMarketplaceUI") == 1, "a second interact sends one more ClientOpenMarketplaceUI");

    // a vendor still opens its window, and only it
    from = h.got.size();
    h.talk_to(h.vendor);
    const auto vendor_calls = pc_calls_since(h, from);
    CHECK(count_calls(vendor_calls, "ClientOpenVendor") == 1 && count_calls(vendor_calls, "ClientOpenMarketplaceUI") == 0, "a vendor still opens ClientOpenVendor, not the marketplace");
    CHECK(h.s->vendor_actor() == h.vendor, "the session's open vendor is the vendor");

    // out of the interact range (40 m) and out of the client's view (600 m): nothing opens
    from = h.got.size();
    h.talk_to(h.far);
    CHECK(count_calls(pc_calls_since(h, from), "ClientOpenMarketplaceUI") == 0, "a Manager 40 m away opens nothing");
    from = h.got.size();
    h.talk_to(h.unseen);
    CHECK(count_calls(pc_calls_since(h, from), "ClientOpenMarketplaceUI") == 0, "a Manager 600 m away opens nothing");
}

static void test_marketplace_open_readable() {
    std::printf("the Marketplace Manager: his open, as a client reads it\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    const world::WorldActor* manager = h.fx.world->registry().find(h.manager);
    CHECK(manager != nullptr && manager->actor.obj.schema && std::string(manager->actor.obj.schema->name) == "ANPCCharacter", "the Manager's actor is the NPC class");
    CHECK(manager != nullptr && manager->npc.interact_def == kManagerInteraction, "his open is the marketplace interaction");
    CHECK(manager != nullptr && manager_open_reads(h, h.manager), "every block of his delivered open reads to its last bit with its class");
}

static void test_marketplace_data_request() {
    std::printf("the Marketplace Manager: the data request with nothing listed\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const auto& cg = h.s->bootstrap().setup().character_guid;
    const repl::GuidValue me{cg[0], cg[1], cg[2], cg[3]};
    size_t from = h.got.size();
    h.client_request(market_headers_request(market, 1843), h.pc_ch);
    auto calls = pc_calls_since(h, from);
    CHECK(!h.s->failed(), "the headers request does not fail the session");
    const repl::RpcCall* headers = nullptr;
    for (const repl::RpcCall& c : calls) {
        if (c.rpc && std::string(c.rpc->name) == "ClientReceiveMarketplaceHeaders") headers = &c;
    }
    CHECK(headers != nullptr && headers->decoded, "the storefront request is answered with the list the client reads");
    if (headers) {
        CHECK(repl::rpcf::value_of(*headers->rpc, headers->params, "Headers") == nullptr, "no storefront header: nothing is listed");
        const auto* complete = repl::rpcf::as<bool>(repl::rpcf::value_of(*headers->rpc, headers->params, "bComplete"));
        CHECK(complete && *complete, "the empty storefront list is complete");
    }
    CHECK(count_calls(calls, "ClientReceiveMarketplaceData") == 0, "the headers request answers headers only");

    from = h.got.size();
    h.client_request(market_data_request(market, me, true), h.pc_ch);
    calls = pc_calls_since(h, from);
    const repl::RpcCall* data = nullptr;
    for (const repl::RpcCall& c : calls) {
        if (c.rpc && std::string(c.rpc->name) == "ClientReceiveMarketplaceData") data = &c;
    }
    CHECK(data != nullptr && data->decoded, "the seller's own data is answered, and reads");
    if (data) {
        CHECK(repl::rpcf::value_of(*data->rpc, data->params, "MarketplaceItemData") == nullptr, "the seller has no listing yet");
    }
    CHECK(count_calls(calls, "ClientReceiveMarketplaceHeaders") == 0, "the data request answers data only");
    from = h.got.size();
    h.talk_to(h.manager);
    CHECK(count_calls(pc_calls_since(h, from), "ClientOpenMarketplaceUI") == 1, "the session still answers the Manager after the request");
}

static void test_marketplace_second_login() {
    std::printf("the Marketplace Manager: the second login\n");
    MarketHarness a;
    a.play();
    a.run(milliseconds(1500));
    size_t from = a.got.size();
    a.talk_to(a.manager);
    const auto calls_a = pc_calls_since(a, from);
    const repl::RpcCall* first = market_open_call(calls_a);
    CHECK(first != nullptr, "the first login gets the Manager's open");
    MarketHarness b;
    b.play();
    b.run(milliseconds(1500));
    from = b.got.size();
    b.talk_to(b.manager);
    const auto calls_b = pc_calls_since(b, from);
    const repl::RpcCall* second = market_open_call(calls_b);
    CHECK(second != nullptr, "the second login gets the Manager's open too");
    if (first && second) {
        const auto word = [](const repl::RpcCall& c, const char* path) { return repl::rpcf::int_or(repl::rpcf::value_of(*c.rpc, c.params, path), 0).value_or(-1); };
        const bool same = word(*first, "MarketplaceGuid.A") == word(*second, "MarketplaceGuid.A") && word(*first, "MarketplaceGuid.B") == word(*second, "MarketplaceGuid.B") &&
                          word(*first, "MarketplaceGuid.C") == word(*second, "MarketplaceGuid.C") && word(*first, "MarketplaceGuid.D") == word(*second, "MarketplaceGuid.D");
        CHECK(same, "both logins get the same market guid");
        const auto* ta = repl::rpcf::as<repl::NodeTaxDetails>(repl::rpcf::value_of(*first->rpc, first->params, "NodeTaxDetails"));
        const auto* tb = repl::rpcf::as<repl::NodeTaxDetails>(repl::rpcf::value_of(*second->rpc, second->params, "NodeTaxDetails"));
        CHECK(ta && tb && ta->node == tb->node && ta->entries.size() == tb->entries.size(), "both logins get the same node's table");
    } else {
        CHECK(false, "both logins get the open call");
    }
}

static void test_marketplace_two_players() {
    std::printf("the Marketplace Manager: two players at one Manager\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    svc::Character second = cyb_record();
    second.id = "0000000000000000000000000000c0b0";
    second.name = "bob";
    MarketPeer peer(h, second);
    peer.play();
    size_t from = peer.got.size();
    peer.request(interact(market_component(h.fx.world->registry(), h.manager)), peer.pawn_ch);
    const auto peer_calls = market_calls_since(peer.got, peer.pc_ch, from);
    CHECK(count_calls(peer_calls, "ClientOpenMarketplaceUI") == 1, "the second player's interact key sends his own one ClientOpenMarketplaceUI");
    const repl::RpcCall* peer_open = market_open_call(peer_calls);
    if (peer_open) check_market_open(*peer_open);

    from = h.got.size();
    h.talk_to(h.manager);
    const auto host_calls = pc_calls_since(h, from);
    CHECK(count_calls(host_calls, "ClientOpenMarketplaceUI") == 1, "the first player's interact key sends his own one ClientOpenMarketplaceUI");
    const repl::RpcCall* host_open = market_open_call(host_calls);
    if (host_open) check_market_open(*host_open);
    if (peer_open && host_open) {
        const auto word = [](const repl::RpcCall& c, const char* path) { return repl::rpcf::int_or(repl::rpcf::value_of(*c.rpc, c.params, path), 0).value_or(-1); };
        CHECK(word(*peer_open, "MarketplaceGuid.A") == word(*host_open, "MarketplaceGuid.A") && word(*peer_open, "MarketplaceGuid.B") == word(*host_open, "MarketplaceGuid.B"),
              "both players get the same market guid");
    } else {
        CHECK(false, "both players get the open call");
    }
}

/// The marketplace's access rule: the calls act only on the Manager the player opened, within its 10 m.
static void test_marketplace_access_window() {
    std::printf("the Marketplace Manager: no window, another market and out of reach\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const auto& cg = h.s->bootstrap().setup().character_guid;
    const repl::GuidValue me{cg[0], cg[1], cg[2], cg[3]};
    const auto quiet = [&](const BunchWriter& request, const char* what) {
        const size_t from = h.got.size();
        h.client_request(request, h.pc_ch);
        const auto calls = pc_calls_since(h, from);
        CHECK(count_calls(calls, "ClientReceiveMarketplaceHeaders") == 0 && count_calls(calls, "ClientReceiveMarketplaceData") == 0, what);
        CHECK(!h.s->failed(), what);
    };
    quiet(market_headers_request(market, 1843), "with no window open the headers request is ignored");
    quiet(market_data_request(market, me, true), "with no window open the data request is ignored");

    h.open_market(h.manager);
    repl::GuidValue another = market;
    another.a += 1;   // a market guid that is not the one the Manager opened
    quiet(market_headers_request(another, 1843), "a headers request for another market is ignored");
    quiet(market_data_request(another, me, true), "a data request for another market is ignored");

    // the Manager at 20 m opens (the interaction range) but the market's own reach is 10 m
    h.open_market(h.edge);
    quiet(market_headers_request(market, 1843), "a headers request from beyond 1000 units is ignored");
    quiet(market_data_request(market, me, true), "a data request from beyond 1000 units is ignored");
}

AOC_REGISTER_TEST(test_marketplace_window);
AOC_REGISTER_TEST(test_marketplace_open_readable);
AOC_REGISTER_TEST(test_marketplace_data_request);
AOC_REGISTER_TEST(test_marketplace_second_login);
AOC_REGISTER_TEST(test_marketplace_two_players);
AOC_REGISTER_TEST(test_marketplace_access_window);
