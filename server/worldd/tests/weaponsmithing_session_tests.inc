// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  weaponsmithing_session_tests.inc: Weaponsmithing through a real session (the Weaponsmithing milestone)
//
//  The real content.db and a station actor made from the captured area: a Novice makes the Brooksteel Axe (the materials leave
//  the bag, 75 copper leave the purse, the axe is an Add with quality 1 and durability 13, Weaponsmithing +240); an Apprentice
//  makes the Golgora pickaxe and it goes to the Pickaxe tool slot 26 (quality 1, durability 300, +1,200 XP), and to the bag when
//  the slot is worn.  A second player at the same station, the same start twice, a reconnect in the middle of a craft and the
//  refusals (a wrong stack, too few materials, a recipe not taught, a station under the recipe's tier) are the other orders.
//  Included by test_worldd_items_session.cpp after its harness; its tests are called from main().
// ============================================================================
#include "spawns/captured_spawns.h"
#include "world/actor_registry.h"
#include "world/city_nodes.h"
#include "world/node_wire.h"
#include "protocol/repl/craft_rpc.h"
#include "protocol/repl/vendor_rpc.h"

namespace wss {
constexpr int64_t kSmith = 108752746862200ll;
constexpr int64_t kSmithT1 = 6064630515162480654ll, kSmithT2 = 6064630519397089280ll;
constexpr int64_t kHalcyon = 108646610851452ll;   // a Village node (level 3, Riverlands)
constexpr int64_t kAxeRecipe = 6064634092895538328ll, kAxeItem = 6064633866903617615ll;
constexpr int64_t kCopper = 6064630417661755489ll, kTimber = 6064629252204724244ll, kAxeReagent = 6064634088230420900ll;
constexpr int64_t kGolgoraRecipe = 6064633764891354166ll, kGolgoraItem = 6064633764670301235ll;
constexpr int64_t kBoardOak = 6064630417661624408ll, kIngotBrass = 6064630417661755493ll, kMoldBasalt = 6064630417661165616ll;
constexpr const char* kT1Tax = "NodeTax.ArtisanTax.SmithyBaseTax.ServiceTax.WeaponsmithServiceTax";
}  // namespace wss

/// The real content.db of the working tree (the one the professions tests use); null when there is none.
static const char* ws_real_db() { return professions_db_path(); }

/// A character to craft with: an empty bag (the tests put what they need in it), money, a certification tier and the recipes it was taught.
static svc::Character ws_character(int tier, std::vector<int64_t> taught, std::vector<svc::InventoryItem> items, const char* name = "cyb", int64_t copper = 100000) {
    svc::Character c;
    c.id = std::string(name) == "cyb" ? kCyb : "bbec5c86eb101fe5f38c87f0fa22e7d1";   // (a character id is 32 hex digits)
    c.account_id = 3;
    c.name = name;
    c.race_id = 0;
    c.gender_id = 0;
    c.class_id = 2;
    c.archetype_id = 17747;
    c.class_name = "Bard";
    c.appearance_json = cyb::kAppearanceJsonb;
    c.level = 1;
    c.items = std::move(items);
    c.wallet.push_back({items::kCopperCurrency, copper});
    if (tier > 0) c.professions.push_back({wss::kSmith, tier, 10, 40000});
    c.known_recipes = std::move(taught);
    return c;
}

/// The station def of a name, and the recipe of a name, in the session's own data.
static const content::CraftStationDef* wss_station(const content::CraftStationData& stations, const char* name) {
    const auto it = stations.by_name.find(name);
    return it == stations.by_name.end() ? nullptr : &stations.defs.at(it->second);
}
static const content::CraftRecipe* wss_recipe(const content::CraftRecipeData& data, const char* name) {
    for (const auto& [guid, r] : data.recipes) {
        if (r.name == name) return &r;
    }
    return nullptr;
}

/// The city nodes and the node wire of the real data: a station's node is a Village (Halcyon, level 3), so the node's biome
/// (Riverlands) and level are the ones the recipes' predicates read.  Loaded once and given to every world of the tests.
struct WsNodes {
    std::unique_ptr<world::CityNodes> nodes;
    std::unique_ptr<world::NodeWire>  wire;
};

static void ws_apply_nodes(Harness& h) {
    static WsNodes kept;
    if (!kept.nodes) {
        std::string error;
        const char* path = ws_real_db();
        if (!path) return;
        kept.nodes = world::CityNodes::load(path, error);
        if (!kept.nodes) {
            std::printf("  [FAIL] the city nodes do not load: %s\n", error.c_str());
            ++g_fail;
            return;
        }
        if (const world::CityNodeInfo* halcyon = kept.nodes->by_name("Halcyon")) kept.nodes->apply_snapshot({{halcyon->guid, 3, 0, 0}});
        kept.wire = world::NodeWire::load(path, *kept.nodes, error);
        if (!kept.wire) {
            std::printf("  [FAIL] the node wire does not load: %s\n", error.c_str());
            ++g_fail;
            return;
        }
    }
    h.fx.world->set_city_nodes(kept.nodes.get());
    h.fx.world->set_node_wire(kept.wire.get());
    h.fx.world->load_crafting_data();
}

/// Replays plan_start for a session's start with the same world facts and prints why it refused (a failure's diagnosis).
static void ws_plan_debug(Harness& h, const emit::FIntrepidNetworkGUID& station_guid, int64_t recipe_guid, const char* recipe_name, const char* station_name,
                          const std::vector<repl::CraftResource>& general) {
    std::string error;
    auto data = h.fx.content->craft_recipe_data(error);
    auto stations = h.fx.content->craft_station_data(error);
    const content::CraftStationDef* def = stations ? wss_station(*stations, station_name) : nullptr;
    const content::CraftRecipe* recipe = data ? wss_recipe(*data, recipe_name) : nullptr;
    if (!data || !def || !recipe) {
        std::printf("    (the diagnosis cannot read the data: %s)\n", error.c_str());
        return;
    }
    crafting::StartFacts f;
    f.recipes = &*data;
    f.station = def;
    f.quick = true;
    f.tier_of = [&](int64_t p) { return h.s->professions().valid() ? h.s->professions().tier(p) : 0; };
    f.taught = [&](int64_t x) { return std::find(h.s->taught_recipes().begin(), h.s->taught_recipes().end(), x) != h.s->taught_recipes().end(); };
    f.tax = crafting::station_tax(def->tax_tag);
    f.node.guid = wss::kHalcyon;
    if (h.fx.world->city_nodes() && h.fx.world->node_wire()) {
        if (const world::CityNodeInfo* node = h.fx.world->city_nodes()->by_guid(f.node.guid)) {
            f.node.level = h.fx.world->city_nodes()->level_of(node->guid);
            f.node.biome = spawns::node_site_of(*h.fx.world->city_nodes(), *h.fx.world->node_wire(), *node, f.node.level).biome;
        }
    }
    f.inventory = &h.s->inventory();
    f.rarity_facts = [&](int64_t item) { return h.fx.world->item_rarity_facts(item); };
    f.copper = h.s->wallet().copper();
    repl::StartCraft req;
    req.station = station_guid;
    req.transaction = {7, 8, 9, 10};
    req.recipe = recipe_guid;
    req.quantity = 1;
    req.general = general;
    const crafting::Plan plan = crafting::plan_start(req, f);
    std::printf("    (the plan of the same facts: ok %d, refusal '%s'; node level %d biome '%s'; tier %d, taught %d, copper %lld)\n", plan.ok ? 1 : 0,
                plan.refusal.c_str(), f.node.level, f.node.biome.c_str(), f.tier_of(recipe->profession), f.taught(recipe->guid) ? 1 : 0,
                static_cast<long long>(f.copper));
}

/// One named stack of a start: the record, how many and the rarity the client names (Common).
static repl::CraftResource wss_resource(int64_t record, int count, int rarity = 0) {
    repl::CraftResource r;
    r.record = record;
    r.rarities.push_back({static_cast<uint8_t>(rarity), count, false});
    return r;
}

/// A crafting station in the session's world, made from the captured area's station and given the definition the test wants.
struct WsStation {
    uint64_t                     id = 0;
    uint64_t                     component = 0;
    emit::FIntrepidNetworkGUID guid;
};

static WsStation ws_add_station(Harness& h, int64_t def, const char* name, const char* tax_tag, double dx, double dy) {
    WsStation out;
    ws_apply_nodes(h);   // the station's node (Halcyon, level 3, Riverlands) is what the recipes' predicates read
    std::string error;
    const auto captured = spawns::CapturedData::load(error);
    if (!captured) {
        std::printf("  [FAIL] the captured data does not load: %s\n", error.c_str());
        ++g_fail;
        return out;
    }
    const spawns::OpenTemplate* t = nullptr;
    for (const spawns::OpenTemplate& o : captured->opens()) {
        if (o.category != "station") continue;
        if (!t) t = &o;
        if (o.archetype_path.find("Weaponsmithing_T2") != std::string::npos) {
            t = &o;
            break;
        }
    }
    if (!t) {
        std::printf("  [FAIL] no captured station template\n");
        ++g_fail;
        return out;
    }
    spawns::InstanceParams p;
    p.x = bootstrap::kDefaultSpawn.x + dx;
    p.y = bootstrap::kDefaultSpawn.y + dy;
    p.z = bootstrap::kDefaultSpawn.z;
    p.server_id = kServerId;
    p.seed = 0x5150u;
    auto w = spawns::instantiate(*t, [&] { return h.fx.guids.next(); }, p, error);
    if (!w) {
        std::printf("  [FAIL] the station does not instantiate: %s\n", error.c_str());
        ++g_fail;
        return out;
    }
    w->station.def = def;
    w->station.name = name;
    w->station.tax_tag = tax_tag;
    w->station.node = wss::kHalcyon;
    w->station.processing = false;
    out.component = w->npc.interact_component;
    out.guid = w->actor.guid;
    out.id = w->id;
    if (!h.fx.world->registry().add(std::move(*w))) {
        std::printf("  [FAIL] the registry refuses the station\n");
        ++g_fail;
        return out;
    }
    return out;
}

/// A client that has phased in: the world's actors (the station among them) are opened to it.
static void ws_phase_in(Harness& h) {
    h.client_sends({h.client.actor(h.pawn_ch, actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});
    h.settle();
}

/// ServerInteractWithInteractableComponent on the station's component, the way the client sends it.
static BunchWriter ws_interact(uint64_t component) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerInteractWithInteractableComponent");
    repl::InteractRequest r;
    r.interactable.ObjectId = component;
    r.interactable.ServerId = kServerId;
    r.interactable.Randomizer = kRandomizer;
    r.counter = 1;
    const auto params = rpc ? repl::interact_params(*rpc, r) : std::nullopt;
    return params ? actor_rpc(pawn, "ServerInteractWithInteractableComponent", *params) : BunchWriter();
}

/// ServerStartCraft of a recipe with the general stacks the client picked; the reagent is not named.
static BunchWriter ws_start_craft(const WsStation& station, int64_t recipe, std::vector<repl::CraftResource> general) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerStartCraft");
    repl::StartCraft s;
    s.station = station.guid;
    s.transaction = {7, 8, 9, 10};
    s.recipe = recipe;
    s.quantity = 1;
    s.general = std::move(general);
    const auto params = rpc ? repl::start_craft_params(*rpc, s) : std::nullopt;
    return params ? actor_rpc(pc, "ServerStartCraft", *params) : BunchWriter();
}

/// The item of `record` in the session's storages (nullopt: it holds none); its container and slot are read by the caller.
static std::optional<svc::InventoryItem> ws_held(const PlayerSession& s, int64_t record) {
    for (const svc::InventoryItem& it : s.inventory().items()) {
        if (it.item_record_id == record) return it;
    }
    return std::nullopt;
}

/// `ms` of play in 20 ms ticks (the session's own).
static void ws_run(Harness& h, int ms) {
    for (int done = 0; done < ms; done += 20) h.step();
}

/// A second player on the same world: its own connection, client and session at the same station.
struct WsPeer {
    Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<PlayerSession> s;
    Clock::time_point t = t0;
    uint32_t pawn_ch = 0, pc_ch = 0;
    std::string error;
    std::vector<fake::Delivered> got;

    bool join(PlayerWorld& world, const svc::Character& rec) {
        s = PlayerSession::join(conn, world, rec, identity(), world_time_at(t), error);
        if (!s) {
            std::printf("  [FAIL] the second player does not join: %s\n", error.c_str());
            ++g_fail;
            return false;
        }
        pawn_ch = s->channel_of(bs::Role::Pawn);
        pc_ch = s->channel_of(bs::Role::PlayerController);
        settle();
        return true;
    }
    void flush() {
        for (auto& pkt : conn.flush(t)) {
            const auto r = client.receive(pkt);
            for (const auto& d : r.delivered) got.push_back(d);
        }
    }
    void client_sends(std::vector<fake::Bunch> bunches) {
        for (const auto& pkt : client.packets(std::move(bunches))) {
            const std::vector<InBunch> in = conn.receive(pkt, t);
            s->take_acks(conn);
            for (const InBunch& b : in) s->on_bunch(conn, b, t);
            s->end_of_datagram(conn);
        }
    }
    void step() {
        t += milliseconds(20);
        s->tick(conn, t);
        flush();
        s->take_acks(conn);
        client_sends({});
    }
    void settle() {
        for (int quiet = 0, i = 0; quiet < 3 && i < 400; ++i) {
            const size_t before = got.size();
            step();
            quiet = (got.size() == before && s->join_pending() == 0 && s->replies_pending() == 0) ? quiet + 1 : 0;
        }
    }
    void request(const BunchWriter& b, uint32_t channel) {
        client_sends({client.actor(channel, b, true)});
        settle();
    }
    void run(int ms) {
        for (int done = 0; done < ms; done += 20) step();
    }
};

static void ws_peer_phase_in(WsPeer& p) {
    p.client_sends({p.client.actor(p.pawn_ch, actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});
    p.settle();
}

/// The storage events the PlayerController's chains `got[from..]` carry, in order.
static std::vector<repl::ItemStorageEvent> ws_storage_events_since(const Harness& h, size_t from) {
    std::vector<repl::ItemStorageEvent> out;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel != h.pc_ch) continue;
        const auto blocks = repl::read_content_blocks(h.got[i].content.data(), h.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            size_t p = b.payload_pos;
            const auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), h.got[i].content.data(), b.payload_pos + b.payload_bits, p);
            if (!calls) continue;
            for (const repl::RpcCall& c : *calls) {
                if (!c.decoded || std::string(c.rpc->name) != "ClientReceiveStorageEvent") continue;
                const auto ev = repl::parse_storage_event(c);
                if (ev) out.push_back(*ev);
            }
        }
    }
    return out;
}

/// The first craft: a Novice's Brooksteel Axe at Novice Weaponsmithing, end to end, with the client's own events.
static void test_ws_session_novice() {
    std::printf("a Novice crafts the Brooksteel Axe at Novice Weaponsmithing\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the Weaponsmithing session tests are skipped\n");
        return;
    }
    std::vector<svc::InventoryItem> items = {make_item(Bag, 0, wss::kCopper, 100, 5), make_item(Bag, 1, wss::kTimber, 101), make_item(Bag, 2, wss::kAxeReagent, 102)};
    Harness h(ws_character(0, {}, std::move(items)), true, true, ws_real_db());
    CHECK(h.s != nullptr, "(joined)");
    if (!h.s) return;
    const WsStation station = ws_add_station(h, wss::kSmithT1, "CraftingStation_Weaponsmithing_T1", wss::kT1Tax, 300, 0);
    CHECK(station.id != 0 && station.component != 0, "a Novice Weaponsmithing station stands 3 m from the player");
    if (station.id == 0) return;
    ws_phase_in(h);
    CHECK(h.s->station_opens() == 0, "no station is open yet");
    h.request(ws_interact(station.component), h.pawn_ch);
    CHECK(h.s->station_opens() == 1, "the client opens the station's window");
    const size_t chains = h.got.size();
    h.request(ws_start_craft(station, wss::kAxeRecipe, {wss_resource(wss::kCopper, 5), wss_resource(wss::kTimber, 1)}), h.pc_ch);
    if (!h.s->quick_active()) {
        std::printf("    (after the start: active %d, started %zu, refused %zu, ignored %zu)\n", h.s->quick_active() ? 1 : 0, h.s->quick_started(), h.s->quick_refused(), h.s->interactions_ignored());
        ws_plan_debug(h, station.guid, wss::kAxeRecipe, "Weaponsmithing_T1_Weapon_Axe_1H_Brooksteel", "CraftingStation_Weaponsmithing_T1",
                      {wss_resource(wss::kCopper, 5), wss_resource(wss::kTimber, 1)});
    }
    CHECK(h.s->quick_active() && h.s->quick_started() == 1, "the craft begins (2 s, nothing is taken until it is made)");
    ws_run(h, 2400);
    h.settle();
    CHECK(h.s->quick_made() == 1 && !h.s->quick_active(), "2 s on, the craft is made");
    const auto axe = ws_held(*h.s, wss::kAxeItem);
    CHECK(axe.has_value(), "the axe is in the bag");
    if (axe) {
        CHECK(axe->container == svc::ItemContainer::Backpack && axe->quality == 1 && std::fabs(axe->durability - 13.0) < 0.01 && axe->quantity == 1,
              "it is quality 1, durability 13, one in its stack, in the Backpack");
    }
    CHECK(!ws_held(*h.s, wss::kCopper) && !ws_held(*h.s, wss::kTimber) && !ws_held(*h.s, wss::kAxeReagent),
          "the 5 Copper MetalFragments, the Ash Timber and the reagent left the bag");
    CHECK(h.s->wallet().copper() == 100000 - 75, "75 copper left the purse (50 and the Smithy service tax of half)");
    const auto state = h.s->professions().state(wss::kSmith);
    CHECK(state.xp == 240 && state.tier == 0, "Weaponsmithing paid 240 experience to the Novice");
    // what the client was told: the Removes of the materials, then the Add of the axe
    int removes = 0, adds = 0;
    for (const repl::ItemStorageEvent& ev : ws_storage_events_since(h, chains)) {
        if (ev.targets.size() != 1 || ev.targets[0].slots.size() != 1) continue;
        if (ev.type == static_cast<uint8_t>(repl::StorageEventType::Remove)) ++removes;
        if (ev.type == static_cast<uint8_t>(repl::StorageEventType::Add)) {
            ++adds;
            CHECK(ev.item.record_id == wss::kAxeItem && ev.item.quality == 1 && std::fabs(ev.item.durability - 13.0) < 0.01,
                  "the client's Add names the axe at quality 1 and durability 13");
        }
    }
    CHECK(removes >= 2 && adds == 1, "the client saw the materials removed and one axe added");
    CHECK(h.mirror.unreadable == 0, "(the client read every chain)");
    CHECK(h.s->wallet().copper() == 100000 - 75, "the Novice's Brooksteel Axe");
}

/// The tool slot: the Apprentice's Golgora pickaxe goes to the Pickaxe slot 26; with the slot worn it goes to the bag.
static void test_ws_session_tool_slot() {
    std::printf("an Apprentice crafts the Golgora pickaxe into the tool slot\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the Weaponsmithing session tests are skipped\n");
        return;
    }
    std::vector<svc::InventoryItem> items = {make_item(Bag, 0, wss::kBoardOak, 200, 10), make_item(Bag, 1, wss::kIngotBrass, 201, 10), make_item(Bag, 2, wss::kMoldBasalt, 202)};
    Harness h(ws_character(1, {wss::kGolgoraRecipe}, std::move(items)), true, true, ws_real_db());
    CHECK(h.s != nullptr && h.s->professions().state(wss::kSmith).tier == 1, "(joined as an Apprentice)");
    if (!h.s) return;
    const WsStation station = ws_add_station(h, wss::kSmithT2, "CraftingStation_Weaponsmithing_T2", wss::kT1Tax, 300, 0);
    if (station.id == 0) return;
    ws_phase_in(h);
    h.request(ws_interact(station.component), h.pawn_ch);
    CHECK(h.s->station_opens() == 1, "the Apprentice Weaponsmithing window opens");
    h.request(ws_start_craft(station, wss::kGolgoraRecipe, {wss_resource(wss::kBoardOak, 10), wss_resource(wss::kIngotBrass, 10), wss_resource(wss::kMoldBasalt, 1)}), h.pc_ch);
    if (!h.s->quick_active()) {
        std::printf("    (after the start: active %d, started %zu, refused %zu, ignored %zu; taught %zu)\n", h.s->quick_active() ? 1 : 0, h.s->quick_started(),
                    h.s->quick_refused(), h.s->interactions_ignored(), h.s->taught_recipes().size());
        ws_plan_debug(h, station.guid, wss::kGolgoraRecipe, "Weaponsmithing_T2_Tool_Pickaxe_Golgora", "CraftingStation_Weaponsmithing_T2",
                      {wss_resource(wss::kBoardOak, 10), wss_resource(wss::kIngotBrass, 10), wss_resource(wss::kMoldBasalt, 1)});
    }
    CHECK(h.s->quick_active() && h.s->quick_started() == 1, "the craft begins");
    ws_run(h, 2400);
    h.settle();
    if (h.s->quick_made() != 1) {   // (the session's reason for a craft that could not be made at its end)
        std::printf("    (after the craft: active %d made %zu interrupted %zu refused %zu)\n", h.s->quick_active() ? 1 : 0, h.s->quick_made(), h.s->quick_interrupted(),
                    h.s->quick_refused());
        const std::string log = g_log.str();
        const size_t at = log.find("cannot be made now");
        if (at != std::string::npos) std::printf("    (the session: %s)\n", log.substr(at, log.find('\n', at) - at).c_str());
        const svc::InventoryItem* slot26 = h.s->inventory().at(svc::ItemContainer::Equipment, items::equip::Pickaxe);
        std::printf("    (the equipment pickaxe slot: %s; boards %lld, ingots %lld, molds %lld)\n", slot26 ? "held" : "free",
                    static_cast<long long>(h.s->inventory().count_materials(wss::kBoardOak, h.fx.world->item_rarity_facts(wss::kBoardOak), 0, items::kRarityCount - 1)),
                    static_cast<long long>(h.s->inventory().count_materials(wss::kIngotBrass, h.fx.world->item_rarity_facts(wss::kIngotBrass), 0, items::kRarityCount - 1)),
                    static_cast<long long>(h.s->inventory().count_materials(wss::kMoldBasalt, h.fx.world->item_rarity_facts(wss::kMoldBasalt), 0, items::kRarityCount - 1)));
        ws_plan_debug(h, station.guid, wss::kGolgoraRecipe, "Weaponsmithing_T2_Tool_Pickaxe_Golgora", "CraftingStation_Weaponsmithing_T2",
                      {wss_resource(wss::kBoardOak, 10), wss_resource(wss::kIngotBrass, 10), wss_resource(wss::kMoldBasalt, 1)});
    }
    CHECK(h.s->quick_made() == 1, "the pickaxe is made");
    const svc::InventoryItem* worn = h.s->inventory().at(svc::ItemContainer::Equipment, items::equip::Pickaxe);
    CHECK(worn != nullptr && worn->item_record_id == wss::kGolgoraItem, "it lies in the equipment's Pickaxe slot 26");
    if (worn) CHECK(worn->quality == 1 && std::fabs(worn->durability - 300.0) < 0.01, "quality 1 and durability 300 (the record is fixed at Rare, whose MaxDurability is 300)");
    CHECK(h.s->professions().state(wss::kSmith).xp == 40000 + 1200, "Weaponsmithing paid 1,200 experience on top of the Apprentice's 40,000 (200 x Rare's 3 x the tool's 2.0 slot factor)");
    CHECK(h.s->wallet().copper() == 100000 - 1500, "1,500 copper left the purse (1,000 and the Smithy service tax of half)");
    // the tool slot is worn: a second pickaxe goes to the bag, and the worn one stays
    Harness full(ws_character(1, {wss::kGolgoraRecipe}, {make_item(Worn, items::equip::Pickaxe, wss::kGolgoraItem, 250), make_item(Bag, 0, wss::kBoardOak, 200, 10),
                                                       make_item(Bag, 1, wss::kIngotBrass, 201, 10), make_item(Bag, 2, wss::kMoldBasalt, 202)}), true, true, ws_real_db());
    const WsStation again = ws_add_station(full, wss::kSmithT2, "CraftingStation_Weaponsmithing_T2", wss::kT1Tax, 300, 0);
    CHECK(again.id != 0, "(a second world with a worn pickaxe)");
    if (again.id == 0) return;
    ws_phase_in(full);
    full.request(ws_interact(again.component), full.pawn_ch);
    full.request(ws_start_craft(again, wss::kGolgoraRecipe, {wss_resource(wss::kBoardOak, 10), wss_resource(wss::kIngotBrass, 10), wss_resource(wss::kMoldBasalt, 1)}), full.pc_ch);
    CHECK(full.s->quick_active(), "with the tool slot worn the craft still begins");
    ws_run(full, 2400);
    full.settle();
    CHECK(full.s->quick_made() == 1, "the second pickaxe is made");
    const svc::InventoryItem* worn_still = full.s->inventory().at(svc::ItemContainer::Equipment, items::equip::Pickaxe);
    CHECK(worn_still != nullptr && worn_still->item_record_id == wss::kGolgoraItem && worn_still->quality == 1, "the worn pickaxe stays in slot 26");
    bool in_bag = false;
    for (const svc::InventoryItem& it : full.s->inventory().items()) in_bag = in_bag || (it.item_record_id == wss::kGolgoraItem && it.container == svc::ItemContainer::Backpack);
    CHECK(in_bag, "the new pickaxe goes to the bag, not over the worn one");
    CHECK(h.s->quick_made() == 1, "the pickaxe's tool slot");
}

/// The other orders: a second player at the same station, the same start twice, a reconnect mid-craft and the refusals.
static void test_ws_session_orders() {
    std::printf("a second player, the same start twice, a reconnect and the refusals\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the Weaponsmithing session tests are skipped\n");
        return;
    }
    // the same start twice: the second is refused while the first runs; after it a second craft with its own set is made
    std::vector<svc::InventoryItem> items = {make_item(Bag, 0, wss::kCopper, 300, 10), make_item(Bag, 1, wss::kTimber, 301, 2),
                                             make_item(Bag, 2, wss::kAxeReagent, 302, 2)};
    Harness h(ws_character(0, {}, std::move(items)), true, true, ws_real_db());
    const WsStation station = ws_add_station(h, wss::kSmithT1, "CraftingStation_Weaponsmithing_T1", wss::kT1Tax, 300, 0);
    if (station.id == 0) return;
    ws_phase_in(h);
    h.request(ws_interact(station.component), h.pawn_ch);
    h.request(ws_start_craft(station, wss::kAxeRecipe, {wss_resource(wss::kCopper, 5), wss_resource(wss::kTimber, 1)}), h.pc_ch);
    CHECK(h.s->quick_active(), "the first craft runs");
    const size_t refused_before = h.s->quick_refused();
    h.request(ws_start_craft(station, wss::kAxeRecipe, {wss_resource(wss::kCopper, 5), wss_resource(wss::kTimber, 1)}), h.pc_ch);
    CHECK(h.s->quick_refused() == refused_before + 1 && h.s->quick_active(), "a second start while it runs is refused, and the first goes on");
    ws_run(h, 2400);
    h.settle();
    CHECK(h.s->quick_made() == 1, "the first craft is made");
    h.request(ws_start_craft(station, wss::kAxeRecipe, {wss_resource(wss::kCopper, 5), wss_resource(wss::kTimber, 1)}), h.pc_ch);
    CHECK(h.s->quick_active() && h.s->quick_started() == 2, "with its own set of materials, the next craft starts");
    ws_run(h, 2400);
    h.settle();
    CHECK(h.s->quick_made() == 2, "and is made: two axes from two sets");

    // a reconnect in the middle: a new session from the same record has no craft and the bag is whole
    std::vector<svc::InventoryItem> before = {make_item(Bag, 0, wss::kCopper, 400, 5), make_item(Bag, 1, wss::kTimber, 401), make_item(Bag, 2, wss::kAxeReagent, 402)};
    Harness mid(ws_character(0, {}, before), true, true, ws_real_db());
    const WsStation mid_station = ws_add_station(mid, wss::kSmithT1, "CraftingStation_Weaponsmithing_T1", wss::kT1Tax, 300, 0);
    CHECK(mid_station.id != 0, "(a third world for the reconnect)");
    if (mid_station.id == 0) return;
    ws_phase_in(mid);
    mid.request(ws_interact(mid_station.component), mid.pawn_ch);
    mid.request(ws_start_craft(mid_station, wss::kAxeRecipe, {wss_resource(wss::kCopper, 5), wss_resource(wss::kTimber, 1)}), mid.pc_ch);
    CHECK(mid.s->quick_active(), "a craft is under way when the connection goes");
    Harness again(ws_character(0, {}, before), true, true, ws_real_db());
    CHECK(again.s != nullptr && !again.s->quick_active(), "a new session from the same record has no craft under way");
    CHECK(ws_held(*again.s, wss::kCopper) && ws_held(*again.s, wss::kTimber) && ws_held(*again.s, wss::kAxeReagent) && again.s->wallet().copper() == 100000,
          "nothing was taken: the bag and the purse are whole (a quick make takes at its end)");

    // the refusals: too few materials, a recipe not taught, a station under its tier, a stack the recipe does not take
    Harness poor(ws_character(0, {}, {make_item(Bag, 0, wss::kCopper, 500, 4), make_item(Bag, 1, wss::kTimber, 501), make_item(Bag, 2, wss::kAxeReagent, 502)}), true, true, ws_real_db());
    const WsStation poor_station = ws_add_station(poor, wss::kSmithT1, "CraftingStation_Weaponsmithing_T1", wss::kT1Tax, 300, 0);
    if (poor_station.id == 0) return;
    ws_phase_in(poor);
    poor.request(ws_interact(poor_station.component), poor.pawn_ch);
    poor.request(ws_start_craft(poor_station, wss::kAxeRecipe, {wss_resource(wss::kCopper, 5), wss_resource(wss::kTimber, 1)}), poor.pc_ch);
    CHECK(poor.s->quick_refused() >= 1 && !poor.s->quick_active(), "5 fragments asked of a bag of 4: refused, nothing taken");
    Harness wrong(ws_character(0, {}, {make_item(Bag, 0, wss::kCopper, 510, 5), make_item(Bag, 1, wss::kTimber, 511), make_item(Bag, 2, wss::kAxeReagent, 512)}), true, true, ws_real_db());
    const WsStation wrong_station = ws_add_station(wrong, wss::kSmithT1, "CraftingStation_Weaponsmithing_T1", wss::kT1Tax, 300, 0);
    if (wrong_station.id == 0) return;
    ws_phase_in(wrong);
    wrong.request(ws_interact(wrong_station.component), wrong.pawn_ch);
    wrong.request(ws_start_craft(wrong_station, wss::kAxeRecipe, {wss_resource(wss::kCopper, 4), wss_resource(wss::kTimber, 2)}), wrong.pc_ch);
    CHECK(wrong.s->quick_refused() >= 1, "the wrong counts for the named stacks are refused (the request names 4 fragments, the recipe takes 5)");
    // the Golgora at the Novice station (tier 1 at a station that reaches 0) and without the learning
    Harness low(ws_character(1, {wss::kGolgoraRecipe}, {make_item(Bag, 0, wss::kBoardOak, 520, 10), make_item(Bag, 1, wss::kIngotBrass, 521, 10),
                                                                  make_item(Bag, 2, wss::kMoldBasalt, 522)}), true, true, ws_real_db());
    const WsStation low_station = ws_add_station(low, wss::kSmithT1, "CraftingStation_Weaponsmithing_T1", wss::kT1Tax, 300, 0);
    if (low_station.id == 0) return;
    ws_phase_in(low);
    low.request(ws_interact(low_station.component), low.pawn_ch);
    low.request(ws_start_craft(low_station, wss::kGolgoraRecipe, {wss_resource(wss::kBoardOak, 10), wss_resource(wss::kIngotBrass, 10), wss_resource(wss::kMoldBasalt, 1)}), low.pc_ch);
    CHECK(low.s->quick_refused() >= 1 && !low.s->quick_active(), "a tier-1 recipe at a station that reaches 0 is refused");
    Harness untaught(ws_character(1, {}, {make_item(Bag, 0, wss::kBoardOak, 530, 10), make_item(Bag, 1, wss::kIngotBrass, 531, 10),
                                                         make_item(Bag, 2, wss::kMoldBasalt, 532)}), true, true, ws_real_db());
    const WsStation untaught_station = ws_add_station(untaught, wss::kSmithT2, "CraftingStation_Weaponsmithing_T2", wss::kT1Tax, 300, 0);
    if (untaught_station.id == 0) return;
    ws_phase_in(untaught);
    untaught.request(ws_interact(untaught_station.component), untaught.pawn_ch);
    untaught.request(ws_start_craft(untaught_station, wss::kGolgoraRecipe, {wss_resource(wss::kBoardOak, 10), wss_resource(wss::kIngotBrass, 10), wss_resource(wss::kMoldBasalt, 1)}), untaught.pc_ch);
    CHECK(untaught.s->quick_refused() >= 1 && !untaught.s->quick_active(), "a recipe the Apprentice was not taught is refused");
    CHECK(h.s->quick_made() == 2, "the other orders");
}

/// Two players at one station: each pays from his own bag and gets his own axe.
static void test_ws_session_second_player() {
    std::printf("a second player at the same station\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the Weaponsmithing session tests are skipped\n");
        return;
    }
    Harness first(ws_character(0, {}, {make_item(Bag, 0, wss::kCopper, 600, 5), make_item(Bag, 1, wss::kTimber, 601), make_item(Bag, 2, wss::kAxeReagent, 602)}),
                  true, true, ws_real_db());
    const WsStation station = ws_add_station(first, wss::kSmithT1, "CraftingStation_Weaponsmithing_T1", wss::kT1Tax, 300, 0);
    CHECK(station.id != 0, "the station stands");
    if (station.id == 0) return;
    WsPeer second;
    CHECK(second.join(*first.fx.world, ws_character(0, {}, {make_item(Bag, 0, wss::kCopper, 610, 5), make_item(Bag, 1, wss::kTimber, 611), make_item(Bag, 2, wss::kAxeReagent, 612)},
                                                       "cyb2")),
          "a second character joins the same world");
    if (!second.s) return;
    ws_phase_in(first);
    ws_peer_phase_in(second);
    first.request(ws_interact(station.component), first.pawn_ch);
    second.request(ws_interact(station.component), second.pawn_ch);
    CHECK(first.s->station_opens() == 1 && second.s->station_opens() == 1, "both players open the same station");
    first.request(ws_start_craft(station, wss::kAxeRecipe, {wss_resource(wss::kCopper, 5), wss_resource(wss::kTimber, 1)}), first.pc_ch);
    second.request(ws_start_craft(station, wss::kAxeRecipe, {wss_resource(wss::kCopper, 5), wss_resource(wss::kTimber, 1)}), second.pc_ch);
    CHECK(first.s->quick_active() && second.s->quick_active(), "both crafts run at the same station");
    ws_run(first, 2400);
    second.run(2400);
    first.settle();
    second.settle();
    CHECK(first.s->quick_made() == 1 && second.s->quick_made() == 1, "both are made");
    CHECK(ws_held(*first.s, wss::kAxeItem).has_value() && ws_held(*second.s, wss::kAxeItem).has_value(), "each player has an axe of his own");
    CHECK(first.s->wallet().copper() == 100000 - 75 && second.s->wallet().copper() == 100000 - 75, "each paid his own 75 copper");
    CHECK(!ws_held(*first.s, wss::kCopper) && !ws_held(*second.s, wss::kCopper) && !ws_held(*first.s, wss::kTimber) && !ws_held(*second.s, wss::kTimber),
          "each bag paid its own materials");
    CHECK(first.s->quick_refused() == 0 && second.s->quick_refused() == 0, "neither was refused: the second player's craft is not the first one's");
}
