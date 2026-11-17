// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tailoring_session_tests.inc: Tailoring through a real session (the Tailoring milestone)
//
//  The real content.db: a Novice opens the Textile Mill vendor of Aven's End at its own NPC asset, buys the AcademicAegis
//  Back's recipe item and the Light reagents at the node's tax, learns the Back by using the item, and makes it at the
//  Tailoring - Novice station (4 Daffodil/Snowdrop thread, a Grem skin, the Light reagent: the materials leave the bag,
//  75 copper leaves the purse, the Back is an Add at quality 1 and durability 11, Tailoring +120).  He wears it and the
//  worn stats follow.  The Hideworks vendor is opened too (the Grem skin's oil).  A second crafter at the same station,
//  a full bag, too little money, a recipe not learned, a station under the recipe's tier, the same craft twice and a
//  reconnect in the middle of a craft are the other orders; the Scribe shirt makes nothing and says so.
//  Included by test_worldd_items_session.cpp after weaponsmithing_session_tests.inc; its tests are called from main().
// ============================================================================
#include "vendor/town_shops.h"

namespace tls {
constexpr int64_t kTailoring = 108752746993277ll;
constexpr int64_t kT1 = 6064630554067599401ll, kT2 = 6064630554071793706ll;
constexpr const char* kTax = "NodeTax.ArtisanTax.TextileMillBaseTax.ServiceTax.TailorStorefrontServiceTax";
constexpr int64_t kTextileMill = 6064630643963592743ll, kHideworks = 6064630643946094627ll;
constexpr int64_t kTextileMillAsset = 6064630656501743649ll, kHideworksAsset = 6064630656625999876ll;
constexpr int64_t kBackRecipe = 6064630990264729600ll, kBackItem = 6064629897568846929ll;
constexpr int64_t kBackTeaching = 6064632254745673788ll;
constexpr int64_t kChestRecipe = 6064629387023482920ll, kChestItem = 109229507958396ll;
constexpr int64_t kScribeRecipe = 6064632236037243106ll, kScribeTeaching = 6064632270186152031ll;
constexpr int64_t kArcaneBackRecipe = 6064629387023482924ll;
constexpr int64_t kThreadSnowdrop = 6064631879480508420ll, kThreadDaffodil = 6064631879482736645ll, kThreadLinen = 6064630417662279814ll;
constexpr int64_t kArtisanShirt = 6064634673789141007ll;
constexpr int64_t kSkinGrem = 6064630417661821030ll, kFleshloosenOil = 6064634002516934663ll;
constexpr int64_t kLightBack = 6064634087930659117ll, kLightChest = 6064634087927906604ll;
constexpr int64_t kSanctusBag = 6064629382556287782ll;   // Bag_Resource_Sanctus: the resource bag every class starts with, in Equipment slot 23
}  // namespace tls

/// A character to craft with: an empty bag (the tests put what they need in it), money, a Tailoring certification tier and the recipes it was taught.
static svc::Character tls_character(int tier, std::vector<int64_t> taught, std::vector<svc::InventoryItem> items, const char* name = "cyb", int64_t copper = 100000, int level = 1) {
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
    c.level = level;
    c.items = std::move(items);
    // the resource bag the reagents need, unless the rows already hold one (a character a session saved and a new one reads)
    bool has_bag = false;
    for (const svc::InventoryItem& it : c.items) has_bag = has_bag || (it.container == svc::ItemContainer::Equipment && it.slot == 23);
    if (!has_bag) c.items.push_back(make_item(Worn, 23, tls::kSanctusBag, 99100));
    c.wallet.push_back({items::kCopperCurrency, copper});
    c.professions.push_back({tls::kTailoring, tier, level, 400LL * level * level});   // the XP of that level on the artisan curve (400 x level squared)
    c.known_recipes = std::move(taught);
    return c;
}

/// A vendor of the artisan outposts in the session's world: a captured vendor NPC given the population asset the data's
/// shop link names, so the shop it opens is the data's.
struct TlsVendor {
    uint64_t                     id = 0;
    uint64_t                     component = 0;
    emit::FIntrepidNetworkGUID   guid;
};

/// The world's town shops, as `PlayerWorld::load_area` gives them: the session's small world is not loaded through the area, so the
/// real vendor tables are handed to it (the shops' predicates still read the city nodes `ws_apply_nodes` set).
static void tls_apply_shops(Harness& h) {
    if (!h.fx.content) return;
    std::string error;
    if (auto data = h.fx.content->vendor_data(error)) h.fx.world->set_town_shops(vendor::TownShops(std::move(*data)));
}

static TlsVendor tls_add_vendor(Harness& h, int64_t asset, const char* name, double dx, double dy) {
    TlsVendor out;
    ws_apply_nodes(h);
    tls_apply_shops(h);
    std::string error;
    const auto captured = spawns::CapturedData::load(error);
    if (!captured) {
        std::printf("  [FAIL] the captured data does not load: %s\n", error.c_str());
        ++g_fail;
        return out;
    }
    const spawns::OpenTemplate* t = captured->template_for(spawns::kGmVendorArchetype);
    if (!t) {
        std::printf("  [FAIL] the captured data has no vendor template\n");
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
        std::printf("  [FAIL] the vendor does not instantiate: %s\n", error.c_str());
        ++g_fail;
        return out;
    }
    // the NPC the data links to the shop: the asset's guid, no instance, no conversation and no GM vendor row
    w->npc.asset_guid = asset;
    w->npc.instance_guid = 0;
    w->npc.is_gm_vendor = false;
    w->npc.vendor_guid = 0;
    w->npc.dialogue_root = 0;
    w->npc.interact_def = 0;
    w->npc.name = name;
    out.component = w->npc.interact_component;
    out.guid = w->actor.guid;
    out.id = w->id;
    if (!h.fx.world->registry().add(std::move(*w))) {
        std::printf("  [FAIL] the registry refuses the vendor\n");
        ++g_fail;
        return out;
    }
    return out;
}

/// ServerHandleVendorPurchase of a cart.
static BunchWriter tls_purchase(const repl::VendorPurchaseCart& cart) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerHandleVendorPurchase");
    const auto params = rpc ? repl::vendor_purchase_params(*rpc, cart) : std::nullopt;
    return params ? actor_rpc(pc, "ServerHandleVendorPurchase", *params) : BunchWriter();
}

/// A cart of (item, quantity) at a shop.
static repl::VendorPurchaseCart tls_cart(int64_t shop, std::vector<std::pair<int64_t, int32_t>> items) {
    repl::VendorPurchaseCart c;
    c.vendor = shop;
    for (const auto& [record, qty] : items) c.items.push_back({record, qty, std::nullopt, std::nullopt});
    return c;
}

/// The item of `record` in the item bag (nullopt when the Backpack holds none; a resource item lies in a resource bag).
static std::optional<svc::InventoryItem> tls_in_bag(const PlayerSession& s, int64_t record) {
    for (const svc::InventoryItem& it : s.inventory().items()) {
        if (it.item_record_id == record && it.container == svc::ItemContainer::Backpack) return it;
    }
    return std::nullopt;
}

/// The item of `record` in any of the character's storages (nullopt when it holds none).
static std::optional<svc::InventoryItem> tls_held(const PlayerSession& s, int64_t record) {
    for (const svc::InventoryItem& it : s.inventory().items()) {
        if (it.item_record_id == record) return it;
    }
    return std::nullopt;
}

/// The quantity of `record` the session holds in all its storages.
static int64_t tls_total(const PlayerSession& s, int64_t record) {
    int64_t n = 0;
    for (const svc::InventoryItem& it : s.inventory().items()) n += it.item_record_id == record ? it.quantity : 0;
    return n;
}

/// A stack of `record` in the bag at `slot`, `quantity` and with a fresh instance guid.
static svc::InventoryItem tls_stack(int slot, int64_t record, int quantity) {
    static uint32_t n = 0;
    svc::InventoryItem it = make_item(Bag, slot, record, ++n + 0x9000);
    it.quantity = quantity;
    it.quality = 1;
    it.durability = 10;
    return it;
}

/// A stack of a resource `record` in the first resource bag at `slot` (the reagents and the processed materials).
static svc::InventoryItem tls_resource(int slot, int64_t record, int quantity) {
    static uint32_t n = 0;
    return make_item(R0, slot, record, ++n + 0x9800, quantity);
}

/// The Tailoring station of the test: the ActorKind::Station, with the profession's Novice definition.
static WsStation tls_station(Harness& h, int64_t def, const char* name) {
    return ws_add_station(h, def, name, tls::kTax, 300, 0);
}

/// Opens the station and starts a recipe with the stacks the client picked (the reagent is not named).
static void tls_start(Harness& h, const WsStation& station, int64_t recipe, std::vector<repl::CraftResource> general) {
    ws_phase_in(h);
    h.request(ws_interact(station.component), h.pawn_ch);
    h.request(ws_start_craft(station, recipe, std::move(general)), h.pc_ch);
}

/// What the first crafted piece shows: an Add of the record at the quality and durability, the last one carrying the craft's id.
static bool tls_add_seen(const Harness& h, size_t from, int64_t record, int quality, double durability, bool last_carries_craft) {
    bool seen = false;
    bool carried = false;
    for (const repl::ItemStorageEvent& ev : ws_storage_events_since(h, from)) {
        if (ev.type != static_cast<uint8_t>(repl::StorageEventType::Add) || ev.targets.size() != 1 || ev.targets[0].slots.size() != 1) continue;
        if (ev.item.record_id != record || ev.item.quality != quality || std::fabs(ev.item.durability - durability) > 0.01) continue;
        seen = true;
        carried = ev.extra_guid_b.has_value() && *ev.extra_guid_b == repl::GuidValue{7, 8, 9, 10};
    }
    (void)last_carries_craft;
    return seen && (!last_carries_craft || carried);
}

/// The whole first case: the vendor, the buy, the learning by the item, the craft, the product and the worn piece.
static void test_tls_vendor_learn_craft() {
    std::printf("a Novice buys the Back's recipe item at the Textile Mill vendor, learns it, makes it and wears it\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the Tailoring session tests are skipped\n");
        return;
    }
    // a level-25 Bard with Tailoring at Journeyman 25.  The thread and the skin are given (the Weaving and Tanning
    // products of the same outpost); the Light reagent and the recipe item are bought at the Textile Mill vendor.
    Harness h(tls_character(2, {}, {tls_stack(0, tls::kThreadSnowdrop, 2), tls_stack(1, tls::kSkinGrem, 1)}, "cyb", 100000, 25), true, true, ws_real_db());
    CHECK(h.s != nullptr, "(joined)");
    if (!h.s) return;
    const TlsVendor textile = tls_add_vendor(h, tls::kTextileMillAsset, "Textile Mill Vendor", 400, 0);
    const TlsVendor hideworks = tls_add_vendor(h, tls::kHideworksAsset, "Hideworks Vendor", 0, 400);
    CHECK(textile.id != 0 && hideworks.id != 0, "the Textile Mill and Hideworks vendors stand 4 m from the player");
    if (textile.id == 0 || hideworks.id == 0) return;
    ws_phase_in(h);
    h.request(ws_interact(textile.component), h.pawn_ch);
    CHECK(h.s->station_opens() == 0 && h.s->vendor_opens() == 1 && h.s->vendor_def() == tls::kTextileMill, "the interaction opens the Textile Mill vendor's window");
    const size_t chains = h.got.size();
    h.request(tls_purchase(tls_cart(tls::kTextileMill, {{tls::kBackTeaching, 1}, {tls::kLightBack, 1}, {tls::kLightChest, 1}})), h.pc_ch);
    if (h.s->vendor_purchases() != 1 || h.s->vendor_refused() != 0) {   // (the diagnosis of a refused cart: the session's reason)
        const std::string log = g_log.str();
        for (size_t at = log.find("[session]"); at != std::string::npos; at = log.find("[session]", at + 1)) {
            const size_t end = log.find('\n', at);
            const std::string line = log.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (line.find("buy") != std::string::npos || line.find("refused") != std::string::npos || line.find("purse") != std::string::npos ||
                line.find("ServerHandleVendorPurchase") != std::string::npos || line.find("does not read") != std::string::npos) {
                std::printf("    %s\n", line.c_str());
            }
        }
        std::printf("    (purchases %zu, refused %zu)\n", h.s->vendor_purchases(), h.s->vendor_refused());
    }
    CHECK(h.s->vendor_purchases() == 1 && h.s->vendor_refused() == 0, "the cart of the recipe item and two Light reagents is applied");
    CHECK(h.s->wallet().copper() == 100000 - 89, "the purse paid 65 for the recipe item and 12 for each reagent");
    const auto teaching = tls_in_bag(*h.s, tls::kBackTeaching);
    CHECK(teaching.has_value() && teaching->quantity == 1, "the recipe item is in the bag");
    CHECK(tls_held(*h.s, tls::kLightBack).has_value() && tls_held(*h.s, tls::kLightChest).has_value(), "the two Light reagents are in the character's bags");
    // the Hideworks vendor: its own shop and its own price for the Grem skin's oil
    h.request(ws_interact(hideworks.component), h.pawn_ch);
    CHECK(h.s->vendor_def() == tls::kHideworks && h.s->vendor_opens() == 2, "the Hideworks vendor opens its own window");
    h.request(tls_purchase(tls_cart(tls::kHideworks, {{tls::kFleshloosenOil, 1}})), h.pc_ch);
    CHECK(h.s->vendor_purchases() == 2 && tls_held(*h.s, tls::kFleshloosenOil).has_value(), "and sells the Grem skin's Fleshloosen Oil");
    CHECK(!ws_storage_events_since(h, chains).empty(), "the client saw the bought stacks in its bag");

    // use the recipe item: the Back is learned, the item is used up
    if (teaching) {
        h.request(item_used(3, teaching->slot, teaching->instance_guid), h.pawn_ch);
        CHECK(std::find(h.s->taught_recipes().begin(), h.s->taught_recipes().end(), tls::kBackRecipe) != h.s->taught_recipes().end(), "using the recipe item teaches the AcademicAegis Back");
        CHECK(!tls_in_bag(*h.s, tls::kBackTeaching).has_value(), "the recipe item is used up");
    }

    // the station and the craft: the thread and the skin are given, the Light Back is the one bought
    const WsStation station = tls_station(h, tls::kT1, "CraftingStation_Tailoring_T1");
    CHECK(station.id != 0, "the Tailoring - Novice station stands 3 m from the player");
    if (station.id == 0) return;
    ws_phase_in(h);   // (the station is added after the phase-in above: the client opens it with this one)
    const int64_t before = h.s->wallet().copper();
    const size_t made_from = h.got.size();
    h.request(ws_interact(station.component), h.pawn_ch);
    CHECK(h.s->station_opens() == 1, "the station's window opens");
    h.request(ws_start_craft(station, tls::kBackRecipe, {wss_resource(tls::kThreadSnowdrop, 2), wss_resource(tls::kSkinGrem, 1)}), h.pc_ch);
    CHECK(h.s->quick_active() && h.s->quick_started() == 1, "the craft begins (2 s)");
    ws_run(h, 2400);
    h.settle();
    CHECK(h.s->quick_made() == 1, "the Back is made");
    const auto back = tls_in_bag(*h.s, tls::kBackItem);
    CHECK(back.has_value() && back->quality == 1 && std::fabs(back->durability - 11.0) < 0.01 && back->quantity == 1, "it is quality 1 with durability 11");
    CHECK(!tls_held(*h.s, tls::kThreadSnowdrop).has_value() && !tls_held(*h.s, tls::kSkinGrem).has_value() && !tls_held(*h.s, tls::kLightBack).has_value(),
          "the 2 thread, the Grem skin and the Light Back left the bags");
    CHECK(h.s->wallet().copper() == before - 75, "75 copper left the purse");
    const auto state = h.s->professions().state(tls::kTailoring);
    std::printf("    (Tailoring: xp %lld, tier %d, level %d)\n", static_cast<long long>(state.xp), state.tier, state.level);
    CHECK(state.xp == 400LL * 25 * 25 + 120, "Tailoring paid 120 experience for the Back (on the level-25 threshold of 250,000)");
    CHECK(tls_add_seen(h, made_from, tls::kBackItem, 1, 11.0, true), "the client's Add names the Back at quality 1 and durability 11, with the craft's id");
    CHECK(h.mirror.unreadable == 0, "(the client read every chain)");

    // wear it: the Back's Common stat block (44 PhysicalMitigationRating, 30 MagicalEvasionRating) reaches the worn stats
    if (back) {
        const int64_t mitigation = guid_named(h, "PhysicalMitigationRating");
        const int64_t evasion = guid_named(h, "MagicalEvasionRating");
        const sg::CharacterStats* cs = h.s->character_stats();
        const auto value = [&](int64_t guid) { return cs && cs->find(guid) ? cs->find(guid)->value : -1.0f; };
        const float before_mitigation = value(mitigation), before_evasion = value(evasion);
        h.request(item_used(3, back->slot, back->instance_guid), h.pawn_ch);
        const svc::InventoryItem* worn = h.s->inventory().at(svc::ItemContainer::Equipment, items::equip::Back);
        CHECK(worn != nullptr && worn->item_record_id == tls::kBackItem, "the crafted Back is worn in the Back slot");
        CHECK(mitigation != 0 && value(mitigation) > before_mitigation, "the Back's PhysicalMitigationRating is a worn stat (it rose)");
        CHECK(evasion != 0 && value(evasion) > before_evasion, "its MagicalEvasionRating is a worn stat (it rose)");
    }
}

/// The other orders: a second crafter at the station, a full bag, too little money, a recipe not learned, a station under
/// the recipe's tier, the same craft twice and a reconnect in the middle.
static void test_tls_orders() {
    std::printf("the Tailoring other orders: a second crafter, a full bag, too little money, the refusals, twice, a reconnect\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the Tailoring session tests are skipped\n");
        return;
    }
    // the same craft twice: the second start while it runs is refused; after it, a second Back from its own set is made
    std::vector<svc::InventoryItem> two = {tls_stack(0, tls::kThreadSnowdrop, 4), tls_stack(1, tls::kSkinGrem, 2), tls_stack(2, tls::kLightBack, 2)};
    Harness h(tls_character(0, {tls::kBackRecipe}, std::move(two)), true, true, ws_real_db());
    const WsStation station = tls_station(h, tls::kT1, "CraftingStation_Tailoring_T1");
    CHECK(h.s != nullptr && station.id != 0, "(the station stands)");
    if (!h.s || station.id == 0) return;
    ws_phase_in(h);
    h.request(ws_interact(station.component), h.pawn_ch);
    h.request(ws_start_craft(station, tls::kBackRecipe, {wss_resource(tls::kThreadSnowdrop, 2), wss_resource(tls::kSkinGrem, 1)}), h.pc_ch);
    CHECK(h.s->quick_active(), "the first craft runs");
    const size_t refused_before = h.s->quick_refused();
    h.request(ws_start_craft(station, tls::kBackRecipe, {wss_resource(tls::kThreadSnowdrop, 2), wss_resource(tls::kSkinGrem, 1)}), h.pc_ch);
    CHECK(h.s->quick_refused() == refused_before + 1 && h.s->quick_active(), "a second start while it runs is refused, and the first goes on");
    ws_run(h, 2400);
    h.settle();
    CHECK(h.s->quick_made() == 1, "the first Back is made");
    h.request(ws_start_craft(station, tls::kBackRecipe, {wss_resource(tls::kThreadSnowdrop, 2), wss_resource(tls::kSkinGrem, 1)}), h.pc_ch);
    CHECK(h.s->quick_active() && h.s->quick_started() == 2, "with its own set, the next craft starts");
    ws_run(h, 2400);
    h.settle();
    CHECK(h.s->quick_made() == 2 && tls_total(*h.s, tls::kBackItem) == 2, "and is made: two Backs from two sets");
    CHECK(h.s->wallet().copper() == 100000 - 150, "each paid its 75 copper");

    // too little money: 74 copper for a 75 copper craft
    Harness poor(tls_character(0, {tls::kBackRecipe}, {tls_stack(0, tls::kThreadSnowdrop, 2), tls_stack(1, tls::kSkinGrem, 1), tls_stack(2, tls::kLightBack, 1)}, "cyb", 74),
                 true, true, ws_real_db());
    const WsStation poor_station = tls_station(poor, tls::kT1, "CraftingStation_Tailoring_T1");
    if (poor_station.id == 0) return;
    tls_start(poor, poor_station, tls::kBackRecipe, {wss_resource(tls::kThreadSnowdrop, 2), wss_resource(tls::kSkinGrem, 1)});
    CHECK(poor.s->quick_refused() >= 1 && !poor.s->quick_active(), "74 copper is refused (the craft costs 75)");
    CHECK(g_log.str().find("the purse holds 74 copper and the job costs 75") != std::string::npos, "the log says the purse is 74 copper short of the 75");
    CHECK(tls_total(*poor.s, tls::kThreadSnowdrop) == 2 && tls_total(*poor.s, tls::kSkinGrem) == 1 && poor.s->wallet().copper() == 74, "nothing was taken");

    // a recipe not learned: the Back without the teaching item
    Harness untaught(tls_character(0, {}, {tls_stack(0, tls::kThreadSnowdrop, 2), tls_stack(1, tls::kSkinGrem, 1), tls_stack(2, tls::kLightBack, 1)}), true, true, ws_real_db());
    const WsStation untaught_station = tls_station(untaught, tls::kT1, "CraftingStation_Tailoring_T1");
    if (untaught_station.id == 0) return;
    tls_start(untaught, untaught_station, tls::kBackRecipe, {wss_resource(tls::kThreadSnowdrop, 2), wss_resource(tls::kSkinGrem, 1)});
    CHECK(untaught.s->quick_refused() >= 1 && !untaught.s->quick_active(), "an unlearned recipe is refused");
    CHECK(g_log.str().find("Tailoring_T1_Armor_AcademicAegis_Back has to be learned, and the character was not taught it") != std::string::npos,
          "the log names the learning");

    // a station under the recipe's tier: an Apprentice recipe at the Novice station (the station's set does not hold it)
    Harness low(tls_character(1, {tls::kArcaneBackRecipe}, {tls_stack(0, tls::kThreadDaffodil, 2), tls_stack(1, tls::kSkinGrem, 1), tls_stack(2, tls::kLightBack, 1)}), true, true, ws_real_db());
    const WsStation low_station = tls_station(low, tls::kT1, "CraftingStation_Tailoring_T1");
    if (low_station.id == 0) return;
    tls_start(low, low_station, tls::kArcaneBackRecipe, {wss_resource(tls::kThreadDaffodil, 2), wss_resource(tls::kSkinGrem, 1)});
    CHECK(low.s->quick_refused() >= 1 && !low.s->quick_active(), "the Apprentice Back at the Novice station is refused");
    CHECK(g_log.str().find("Tailoring_T2_Armor_ArcaneAegis_Back is not one of the recipes of Tailoring - Novice") != std::string::npos, "the log names the station's recipes");

    // a full bag: the chest needs a new item-bag slot at the end (the materials lie in the resource bags)
    std::vector<svc::InventoryItem> full = {tls_resource(0, tls::kThreadDaffodil, 8), tls_resource(1, tls::kSkinGrem, 2), tls_resource(2, tls::kLightChest, 2)};
    for (int i = 0; i < items::kBackpackSlots; ++i) full.push_back(tls_stack(i, tls::kBackItem, 1));
    Harness bag(tls_character(0, {}, std::move(full)), true, true, ws_real_db());
    const WsStation bag_station = tls_station(bag, tls::kT1, "CraftingStation_Tailoring_T1");
    CHECK(bag.s != nullptr && bag.s->inventory().count(svc::ItemContainer::Backpack) == static_cast<size_t>(items::kBackpackSlots), "(a full bag)");
    if (!bag.s || bag_station.id == 0) return;
    tls_start(bag, bag_station, tls::kChestRecipe, {wss_resource(tls::kThreadDaffodil, 4), wss_resource(tls::kSkinGrem, 1)});
    CHECK(bag.s->quick_active(), "the craft begins: the room for the product is looked at when it ends");
    ws_run(bag, 2400);
    bag.settle();
    CHECK(bag.s->quick_made() == 0 && !bag.s->quick_active() && bag.s->quick_interrupted() >= 1, "the full bag ends the craft with nothing made");
    CHECK(tls_total(*bag.s, tls::kThreadDaffodil) == 8 && tls_total(*bag.s, tls::kSkinGrem) == 2 && tls_total(*bag.s, tls::kLightChest) == 2 && bag.s->wallet().copper() == 100000,
          "nothing was taken and nothing paid");
    CHECK(g_log.str().find("the bags have no room") != std::string::npos, "the log says the bags had no room");

    // a reconnect in the middle: the materials and the purse are whole in what was saved, and the same craft is made after
    const std::vector<svc::InventoryItem> before = {tls_stack(0, tls::kThreadSnowdrop, 2), tls_stack(1, tls::kSkinGrem, 1), tls_stack(2, tls::kLightBack, 1)};
    Harness mid(tls_character(0, {tls::kBackRecipe}, before), true, true, ws_real_db());
    const WsStation mid_station = tls_station(mid, tls::kT1, "CraftingStation_Tailoring_T1");
    if (mid_station.id == 0) return;
    tls_start(mid, mid_station, tls::kBackRecipe, {wss_resource(tls::kThreadSnowdrop, 2), wss_resource(tls::kSkinGrem, 1)});
    CHECK(mid.s->quick_active(), "the craft is under way when the connection goes");
    ws_run(mid, 1000);
    mid.s->finish("the connection was lost");
    CHECK(mid.s->quick_made() == 0 && mid.s->wallet().copper() == 100000, "the end of the session makes nothing and takes nothing");
    bool whole = true;
    for (const RecordingSink::Saved& saved : mid.sink.inventories) {
        int64_t thread = 0, skin = 0, reagent = 0, backs = 0;
        for (const svc::InventoryItem& it : saved.items) {
            thread += it.item_record_id == tls::kThreadSnowdrop ? it.quantity : 0;
            skin += it.item_record_id == tls::kSkinGrem ? it.quantity : 0;
            reagent += it.item_record_id == tls::kLightBack ? it.quantity : 0;
            backs += it.item_record_id == tls::kBackItem ? it.quantity : 0;
        }
        whole = whole && thread == 2 && skin == 1 && reagent == 1 && backs == 0;
    }
    CHECK(whole, "every save holds the whole materials and no Back");
    const std::vector<svc::InventoryItem> rows = mid.sink.inventories.empty() ? before : mid.sink.inventories.back().items;
    Harness again(tls_character(0, {tls::kBackRecipe}, rows), true, true, ws_real_db());
    const WsStation again_station = tls_station(again, tls::kT1, "CraftingStation_Tailoring_T1");
    if (again_station.id == 0) return;
    CHECK(!again.s->quick_active() && again.s->quick_made() == 0, "a new session has no craft under way");
    tls_start(again, again_station, tls::kBackRecipe, {wss_resource(tls::kThreadSnowdrop, 2), wss_resource(tls::kSkinGrem, 1)});
    ws_run(again, 2400);
    again.settle();
    CHECK(again.s->quick_made() == 1 && tls_in_bag(*again.s, tls::kBackItem).has_value() && again.s->wallet().copper() == 100000 - 75, "the same craft is made after the reconnect: the Back, 75 copper");
}

/// Two players at one station: each pays from his own bag and gets his own Back.
static void test_tls_second_player() {
    std::printf("a second crafter at the same Tailoring station\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the Tailoring session tests are skipped\n");
        return;
    }
    Harness first(tls_character(0, {tls::kBackRecipe}, {tls_stack(0, tls::kThreadSnowdrop, 2), tls_stack(1, tls::kSkinGrem, 1), tls_stack(2, tls::kLightBack, 1)}),
                  true, true, ws_real_db());
    const WsStation station = tls_station(first, tls::kT1, "CraftingStation_Tailoring_T1");
    CHECK(first.s != nullptr && station.id != 0, "the station stands");
    if (!first.s || station.id == 0) return;
    WsPeer second;
    CHECK(second.join(*first.fx.world, tls_character(0, {tls::kBackRecipe},
                                                     {tls_stack(0, tls::kThreadSnowdrop, 2), tls_stack(1, tls::kSkinGrem, 1), tls_stack(2, tls::kLightBack, 1)}, "cyb2")),
          "a second character joins the same world");
    if (!second.s) return;
    ws_phase_in(first);
    ws_peer_phase_in(second);
    first.request(ws_interact(station.component), first.pawn_ch);
    second.request(ws_interact(station.component), second.pawn_ch);
    CHECK(first.s->station_opens() == 1 && second.s->station_opens() == 1, "both crafters open the same station");
    first.request(ws_start_craft(station, tls::kBackRecipe, {wss_resource(tls::kThreadSnowdrop, 2), wss_resource(tls::kSkinGrem, 1)}), first.pc_ch);
    second.request(ws_start_craft(station, tls::kBackRecipe, {wss_resource(tls::kThreadSnowdrop, 2), wss_resource(tls::kSkinGrem, 1)}), second.pc_ch);
    CHECK(first.s->quick_active() && second.s->quick_active(), "both crafts run at the same station");
    ws_run(first, 2400);
    second.run(2400);
    first.settle();
    second.settle();
    CHECK(first.s->quick_made() == 1 && second.s->quick_made() == 1, "both Backs are made");
    CHECK(tls_in_bag(*first.s, tls::kBackItem).has_value() && tls_in_bag(*second.s, tls::kBackItem).has_value(), "each crafter has a Back of his own");
    CHECK(first.s->wallet().copper() == 100000 - 75 && second.s->wallet().copper() == 100000 - 75, "each paid his own 75 copper");
    CHECK(!tls_held(*first.s, tls::kThreadSnowdrop).has_value() && !tls_held(*second.s, tls::kThreadSnowdrop).has_value() &&
              !tls_held(*first.s, tls::kSkinGrem).has_value() && !tls_held(*second.s, tls::kSkinGrem).has_value(),
          "each character paid his own materials");
}

/// The six artisan shirts that make nothing: the Scribe shirt is learned, started and refused at the reward with the reason.
static void test_tls_empty_reward() {
    std::printf("a recipe whose reward gives nothing: the Scribe shirt\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the Tailoring session tests are skipped\n");
        return;
    }
    Harness h(tls_character(0, {tls::kScribeRecipe}, {tls_stack(0, tls::kThreadLinen, 9), tls_stack(1, tls::kArtisanShirt, 1)}), true, true, ws_real_db());
    const WsStation station = tls_station(h, tls::kT1, "CraftingStation_Tailoring_T1");
    CHECK(h.s != nullptr && station.id != 0, "(the station stands)");
    if (!h.s || station.id == 0) return;
    tls_start(h, station, tls::kScribeRecipe, {wss_resource(tls::kThreadLinen, 9)});
    CHECK(h.s->quick_refused() >= 1 && !h.s->quick_active(), "the Scribe shirt does not start");
    CHECK(g_log.str().find("Tailoring_T1_Artisan_Scribe_Shirt has no reward the server can give: it makes nothing") != std::string::npos,
          "the log says the recipe makes nothing");
    CHECK(h.s->wallet().copper() == 100000 && tls_total(*h.s, tls::kThreadLinen) == 9 && tls_total(*h.s, tls::kArtisanShirt) == 1, "nothing was taken");
}
