// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/items_session_recipe_shops_outposts_tests.inc: the Laboratory and Stoneworks vendors of the artisan outposts (the recipe shops milestone, step 1)
//  (registered with AOC_REGISTER_TEST: test_worldd_items_session.cpp runs it through aoc_tests::run; it is included from items_session_registered.inc)
//
//  The Laboratory vendor (the Alchemy, Arcane Engineering and Scribe tabs: 40 listings) and the Stoneworks vendor (the Jeweler, Mining and Stonemasonry tabs: 14 listings) of Aven's End and
//  Briarmoor Workshops are open shops now.  Every listing is in money, none has an availability condition, and five of them are recipe items (two Scribe, three Jeweler) at 43 copper.  The data
//  tests read the real content.db through the shops' own quote; the session tests use the real shopkeepers' links (the asset and the instance of the data), a Scribe and a Jeweler at
//  Novice, and the real purchase path: the window opens, the node's taxes are answered, the cart is bought whole or refused whole, and the recipe item teaches its recipe when used.
//  Included by items_session_registered.inc, after tailoring_session_tests.inc and tailoring_review_session_tests.inc (their helpers are used).
// ============================================================================
#include "protocol/repl/rpc_fields.h"
#include "vendor/town_shops.h"

namespace rso {
constexpr int64_t kLaboratory = 6064630643947798564ll, kStoneworks = 6064630643962216486ll;
constexpr int64_t kLaboratoryAsset = 6064630656484376606ll, kStoneworksAsset = 6064630656400883736ll;
constexpr int64_t kLaboratoryInstance = 6064630656486277151ll, kStoneworksInstance = 6064630656403308569ll;
constexpr int64_t kScribe = 108752746993279ll, kJeweler = 108752746993278ll;
constexpr int64_t kNovice = 108906339653240ll;   // the certification the five recipe items ask for

struct RecipeItem {
    int64_t     item;
    int64_t     recipe;
    const char* name;
};
constexpr RecipeItem kScribeItems[] = {{6064630446995210598ll, 6064630418886428338ll, "Consumable_Recipe_Scribe_T1_Scroll_Intelligence"},
                                       {6064630446995210596ll, 6064630418886362800ll, "Consumable_Recipe_Scribe_T1_Scroll_Strength"}};
constexpr RecipeItem kJewelerItems[] = {{6064632224513916929ll, 6064630418265212524ll, "Consumable_Recipe_Jeweler_T1_Earring_Flowmarked"},
                                        {6064632224513916931ll, 6064630418265212527ll, "Consumable_Recipe_Jeweler_T1_Necklace_Flowmarked"},
                                        {6064632224513916933ll, 6064630418265278066ll, "Consumable_Recipe_Jeweler_T1_Ring_Flowmarked"}};
constexpr int64_t kIntelligenceItem = 6064630446995210598ll, kIntelligenceRecipe = 6064630418886428338ll;
constexpr int64_t kStrengthItem = 6064630446995210596ll, kStrengthRecipe = 6064630418886362800ll;
constexpr int64_t kRingItem = 6064632224513916933ll, kRingRecipe = 6064630418265278066ll;
constexpr int64_t kCrudePaper = 6064630417666605424ll;        // Resource_Reagent_CrudePaper: 34 copper in the Scribe and Jeweler tabs
constexpr int64_t kEnchantingEssence = 6064633385240035328ll; // Resource_Reagent_EnchantingEssence: 8 copper
constexpr int64_t kWater = 6064630417665753417ll;             // Resource_Reagent_Water: 8 copper

// the shops' price with the node's default tax (50 percent of the price, rounded up): a recipe item 43 + 22, a reagent of 8 copper 8 + 4, the crude paper 34 + 17
constexpr int64_t kRecipeItemCost = 65, kReagentCost = 12, kCrudePaperCost = 51;
}  // namespace rso

// ---- helpers ---------------------------------------------------------------------------------------------------------------------

/// A character with `profession` at Novice (the certification the recipe items ask for), the recipes it was taught, the stacks it holds and the purse.
static svc::Character rso_character(int64_t profession, std::vector<int64_t> taught, std::vector<svc::InventoryItem> items, int64_t copper = 100000) {
    svc::Character c = tls_character(0, std::move(taught), std::move(items), "cyb", copper, 10);
    c.professions.clear();
    c.professions.push_back({profession, 0, 10, 400LL * 10 * 10});
    return c;
}

/// ServerGetNodeTaxTable of a node: what the vendor window sends as soon as it opens.
static BunchWriter rso_tax_request(int64_t node) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerGetNodeTaxTable");
    const auto params = rpc ? repl::node_tax_request_params(*rpc, node) : std::nullopt;
    return params ? actor_rpc(pc, "ServerGetNodeTaxTable", *params) : BunchWriter();
}

/// The PlayerController's decoded calls among the chains `h.got[from..]`, in order.
static std::vector<repl::RpcCall> rso_calls_since(const Harness& h, size_t from) {
    std::vector<repl::RpcCall> out;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel != h.pc_ch || h.got[i].open || h.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(h.got[i].content.data(), h.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            if (!b.header.is_actor || b.header.has_rep_layout) continue;
            size_t p = b.payload_pos;
            auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), h.got[i].content.data(), b.payload_pos + b.payload_bits, p);
            if (calls) {
                for (auto& c : *calls) out.push_back(std::move(c));
            }
        }
    }
    return out;
}

static size_t rso_count(const std::vector<repl::RpcCall>& calls, const char* name) {
    return static_cast<size_t>(std::count_if(calls.begin(), calls.end(), [&](const repl::RpcCall& c) { return c.rpc && std::string(c.rpc->name) == name; }));
}

/// The ClientOpenVendor among `calls` names `shop` and the shopkeeper `guid`.
static bool rso_opens(const std::vector<repl::RpcCall>& calls, int64_t shop, const emit::FIntrepidNetworkGUID& guid) {
    for (const repl::RpcCall& c : calls) {
        if (!c.rpc || std::string(c.rpc->name) != "ClientOpenVendor") continue;
        const auto o = repl::parse_open_vendor(c);
        if (o && o->merch_set == shop && o->vendor == guid) return true;
    }
    return false;
}

/// A shopkeeper the way the world spawns one: the data's instance link and no asset (the instance's link wins over its asset's).
static void rso_link_by_instance(Harness& h, const TlsVendor& v, int64_t instance) {
    h.fx.world->registry().modify(v.id, world::Change::State, [instance](world::WorldActor& w) {
        w.npc.instance_guid = instance;
        w.npc.asset_guid = 0;
    });
}

/// What a request may change, to see that a refused one changed none of it.
struct RsoState {
    size_t  purchases = 0, refused = 0, items = 0;
    int64_t copper = 0;
};
static RsoState rso_state(const Harness& h) { return {h.s->vendor_purchases(), h.s->vendor_refused(), h.s->inventory().items().size(), h.s->wallet().copper()}; }

/// A cart sent to the open window is refused whole: counted, nothing paid or given, no cost shown to the client, and the log says why (`reason` is a part of the text).
static void rso_expect_refused(Harness& h, const repl::VendorPurchaseCart& cart, const char* reason, const std::string& what) {
    const RsoState before = rso_state(h);
    const size_t mark = static_cast<size_t>(g_log.tellp());
    const size_t from = h.got.size();
    const BunchWriter request = tls_purchase(cart);
    h.request(request, h.pc_ch);
    const RsoState after = rso_state(h);
    const bool whole = after.purchases == before.purchases && after.refused == before.refused + 1 && after.copper == before.copper && after.items == before.items &&
                       rso_count(rso_calls_since(h, from), "ClientShowCurrencyCallout") == 0;
    std::string diagnosis;   // (only for a failing check: what the request was and what the session logged)
    if (!whole) {
        std::string tail = g_log.str().substr(mark, 400);
        std::replace(tail.begin(), tail.end(), static_cast<char>(10), ' ');
        diagnosis = " [request " + std::to_string(request.bit_pos()) + " bits; purchases " + std::to_string(before.purchases) + "->" + std::to_string(after.purchases) + ", refused " +
                    std::to_string(before.refused) + "->" + std::to_string(after.refused) + ", copper " + std::to_string(before.copper) + "->" + std::to_string(after.copper) + ", items " +
                    std::to_string(before.items) + "->" + std::to_string(after.items) + "; log: " + tail + "]";
    }
    CHECK(whole, (what + ": refused whole, counted, nothing paid, nothing given, no cost shown" + diagnosis).c_str());
    CHECK(g_log.str().find(reason, mark) != std::string::npos, (what + ": the log says why (" + reason + ")").c_str());
}

/// Opens the shop's window by using the shopkeeper; false when the window of `shop` is not the open one afterwards.
static bool rso_open_window(Harness& h, const TlsVendor& v, int64_t shop) {
    h.request(ws_interact(v.component), h.pawn_ch);
    return h.s->vendor_def() == shop;
}

/// The vendors' shops from the real content.db.
static std::optional<vendor::TownShops> rso_shops(const content::ContentDb& db) {
    std::string error;
    auto data = db.vendor_data(error);
    if (!data) {
        std::printf("  [FAIL] the shops do not read: %s\n", error.c_str());
        ++g_fail;
        return std::nullopt;
    }
    return std::optional<vendor::TownShops>(std::in_place, std::move(*data));
}

// ---- the data: the shops are open, every listing prices ---------------------------------------------------------------------------

void test_rso_shops_are_open_and_priced() {
    std::printf("recipe shops 1: the Laboratory and Stoneworks vendors are open shops and every listing of their six tabs is priced in money\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the recipe shop tests are skipped\n");
        return;
    }
    Fixture fx(true, ws_real_db());
    CHECK(fx.content != nullptr, "(the real content.db opens)");
    if (!fx.content) return;
    const auto shops_opt = rso_shops(*fx.content);
    if (!shops_opt) return;
    const vendor::TownShops& shops = *shops_opt;
    const content::VendorData::Def* lab = shops.def(rso::kLaboratory);
    const content::VendorData::Def* stone = shops.def(rso::kStoneworks);
    CHECK(lab && lab->name == "ArtisanshipPOI_Laboratory_Vendor" && stone && stone->name == "ArtisanshipPOI_Stoneworks_Vendor", "the Laboratory vendor and the Stoneworks vendor are in the data");
    if (!lab || !stone) return;
    CHECK(shops.is_open(rso::kLaboratory) && shops.is_open(rso::kStoneworks), "both shops are switched on");
    CHECK(shops.open_shops().size() == 10, "ten shops are open: the four of the towns and the six of the artisan outposts");
    CHECK(!lab->blacklisted && !stone->blacklisted && (lab->operations & 1) && (stone->operations & 1), "both sell (neither is blacklisted)");
    CHECK(lab->inventories.size() == 3 && stone->inventories.size() == 3, "each has three tabs (Alchemy, Arcane Engineering and Scribe; Jeweler, Mining and Stonemasonry)");

    // the listings: 40 and 14, every one priced in money, none behind a condition (a Textile Mill listing is gated by the node's biome; these are not)
    size_t lab_rows = 0, stone_rows = 0, money = 0, recipe_items = 0;
    std::set<int64_t> taught;
    for (const int64_t shop : {rso::kLaboratory, rso::kStoneworks}) {
        const auto prices = shops.data().prices.find(shop);
        if (prices == shops.data().prices.end()) continue;
        for (const auto& [item, rows] : prices->second) {
            for (const content::VendorPriceRow& r : rows) {
                (shop == rso::kLaboratory ? lab_rows : stone_rows) += 1;
                money += r.currency == items::kMoneyTier && r.variation == 0 && r.offer == 0 ? 1 : 0;
            }
            std::string error;
            const auto learn = fx.content->item_learn(item, error);
            if (learn && !learn->empty()) {
                ++recipe_items;
                for (const content::ItemLearnRow& row : *learn) taught.insert(row.recipe);
            }
        }
    }
    CHECK(lab_rows == 40 && stone_rows == 14, "the Laboratory vendor lists 40 and the Stoneworks vendor 14");
    CHECK(money == 54, "all 54 listings are priced in money, with no variation and no offer");
    CHECK(recipe_items == 5 && taught.size() == 5, "five of the listings are recipe items, and they teach five recipes");
    size_t conditions = 0;
    for (const int64_t shop : {rso::kLaboratory, rso::kStoneworks}) {
        for (const int64_t tab : shops.def(shop)->inventories) {
            const auto it = shops.data().predicates.lower_bound({tab, -1});
            conditions += it != shops.data().predicates.end() && it->first.first == tab ? 1 : 0;
        }
    }
    const vendor::TownShops::Holds none_hold = [](const content::VendorPredicateRow&) { return false; };
    CHECK(conditions == 0 && shops.unavailable(rso::kLaboratory, none_hold).empty() && shops.unavailable(rso::kStoneworks, none_hold).empty(),
          "no tab and no listing has an availability condition: the windows hide nothing wherever the shopkeeper stands");

    // every shopkeeper the data links to them opens them (the instance's link first, the asset's otherwise)
    size_t lab_instances = 0, stone_instances = 0, lab_assets = 0, stone_assets = 0, opening = 0;
    for (const auto& [instance, shop] : shops.data().instance_links) {
        if (shop != rso::kLaboratory && shop != rso::kStoneworks) continue;
        (shop == rso::kLaboratory ? lab_instances : stone_instances) += 1;
        opening += shops.shop_of(instance, 0) == shop ? 1 : 0;
    }
    for (const auto& [asset, shop] : shops.data().asset_links) {
        if (shop != rso::kLaboratory && shop != rso::kStoneworks) continue;
        (shop == rso::kLaboratory ? lab_assets : stone_assets) += 1;
        opening += shops.shop_of(0, asset) == shop ? 1 : 0;
    }
    CHECK(lab_instances == 4 && stone_instances == 4 && lab_assets == 1 && stone_assets == 1 && opening == 10, "each vendor has an asset and four instances in the data, and all ten open their shop");
    CHECK(shops.shop_of(rso::kLaboratoryInstance, 0) == rso::kLaboratory && shops.shop_of(0, rso::kLaboratoryAsset) == rso::kLaboratory && shops.shop_of(rso::kStoneworksInstance, 0) == rso::kStoneworks &&
              shops.shop_of(0, rso::kStoneworksAsset) == rso::kStoneworks,
          "the two shopkeepers the tests use open their own shops");

    // the five recipe items: listed once at 43 copper in their shop, each teaching a live Novice recipe of the right profession
    std::string error;
    const auto recipes = fx.content->craft_recipe_data(error);
    CHECK(recipes.has_value(), "the recipes read");
    if (!recipes) return;
    const struct {
        int64_t           shop, profession;
        const rso::RecipeItem* items;
        size_t            count;
    } sets[] = {{rso::kLaboratory, rso::kScribe, rso::kScribeItems, 2}, {rso::kStoneworks, rso::kJeweler, rso::kJewelerItems, 3}};
    size_t listed = 0, learns = 0, live = 0;
    for (const auto& set : sets) {
        const auto prices = shops.data().prices.find(set.shop);
        for (size_t i = 0; i < set.count; ++i) {
            const rso::RecipeItem& ri = set.items[i];
            if (prices != shops.data().prices.end()) {
                const auto item = prices->second.find(ri.item);
                listed += item != prices->second.end() && item->second.size() == 1 && item->second.front().price == 43 ? 1 : 0;
            }
            const auto learn = fx.content->item_learn(ri.item, error);
            if (learn && learn->size() == 1 && learn->front().recipe == ri.recipe && learn->front().profession == set.profession && learn->front().certification == rso::kNovice) ++learns;
            const content::CraftRecipe* r = recipes->recipe(ri.recipe);
            live += r && r->learnable && !r->blacklisted && r->profession == set.profession && r->tier == 0 ? 1 : 0;
        }
    }
    CHECK(listed == 5, "the five recipe items are listed once each, for 43 copper before the tax");
    CHECK(learns == 5, "each teaches one recipe, of its profession, at the Novice certification");
    CHECK(live == 5, "the five recipes are live: a Novice recipe the character has to learn, not blacklisted");

    // every listing of the six tabs prices through the shops' own quote: 54 carts of one, then a cart of 70 lines (the most) and one of 71
    const vendor::TaxRate rate = shops.default_rate(rso::kLaboratory);
    CHECK(rate.found && rate.percent == 0.5f && rate.flat == 0 && vendor::taxed_unit(43, rate) == rso::kRecipeItemCost && vendor::taxed_unit(8, rate) == rso::kReagentCost &&
              vendor::taxed_unit(34, rate) == rso::kCrudePaperCost,
          "the node's default tax is half the price rounded up: 43 copper costs 65, 8 costs 12, 34 costs 51");
    const vendor::TownShops::Holds all_hold = [](const content::VendorPredicateRow&) { return true; };
    size_t quoted = 0, taxed_right = 0;
    for (const int64_t shop : {rso::kLaboratory, rso::kStoneworks}) {
        const auto prices = shops.data().prices.find(shop);
        for (const auto& [item, rows] : prices->second) {
            repl::VendorPurchaseCart cart = tls_cart(shop, {{item, 1}});
            for (const vendor::TownShops::Holds* holds : {&all_hold, &none_hold}) {
                const vendor::TownShops::Quote q = shops.quote(shop, cart, shops.default_rate(shop), *holds);
                const bool ok = q.ok && q.lines.size() == 1 && q.currency == items::kMoneyTier && q.total == vendor::taxed_unit(rows.front().price, shops.default_rate(shop));
                quoted += ok ? 1 : 0;
                taxed_right += ok && q.lines.front().unit == q.total ? 1 : 0;
            }
        }
    }
    CHECK(quoted == 108 && taxed_right == 108, "each of the 54 listings quotes (asked with every condition holding and with none holding): its price plus the node's tax, in money");
    {
        repl::VendorPurchaseCart cart = tls_cart(rso::kLaboratory, {});
        for (int i = 0; i < 70; ++i) cart.items.push_back({rso::kWater, 1, std::nullopt, std::nullopt});
        const vendor::TownShops::Quote at_most = shops.quote(rso::kLaboratory, cart, rate, all_hold);
        cart.items.push_back({rso::kWater, 1, std::nullopt, std::nullopt});
        const vendor::TownShops::Quote too_many = shops.quote(rso::kLaboratory, cart, rate, all_hold);
        CHECK(at_most.ok && at_most.total == 70 * rso::kReagentCost && !too_many.ok && too_many.refusal.find("71 lines") != std::string::npos, "a cart of 70 lines quotes and a cart of 71 is refused");
    }

    // what stays closed: the Barracks, the citizen outfitter, the mayor's shop, the artisan buildings' vendors and the PTR vendor defs
    size_t named_closed = 0, ptr = 0, ptr_open = 0;
    for (const auto& [guid, def] : shops.data().defs) {
        if (def.name == "Barracks_Vendor" || def.name == "Node_Citizen_Vendor" || def.name == "Node_Mayor_Vendor" || def.name == "ServiceBuilding_Laboratory_Scribe_Vendor" ||
            def.name == "ServiceBuilding_Stoneworks_Jeweler_Vendor" || def.name == "ServiceBuilding_Laboratory_Alchemy_Vendor" || def.name == "ServiceBuilding_TextileMill_Tailoring_Vendor") {
            named_closed += shops.is_open(guid) ? 0 : 1;
        }
        if (def.name.rfind("PTR_ONLY_Vendor_", 0) == 0) {
            ++ptr;
            ptr_open += shops.is_open(guid) ? 1 : 0;
        }
    }
    CHECK(named_closed == 7, "the Barracks, the citizen outfitter, the mayor's shop and four artisan-building vendors stay closed");
    CHECK(ptr > 0 && ptr_open == 0, "no PTR vendor def is one of the open shops (they are no recipe source)");
}
AOC_REGISTER_TEST(test_rso_shops_are_open_and_priced);

// ---- the Laboratory vendor: window, taxes, buy, learn, relog --------------------------------------------------------------------

void test_rso_laboratory_buy_learn_relog() {
    std::printf("recipe shops 1: at the Laboratory vendor a Scribe opens the window, buys the recipe items for 43 copper and the node's tax, learns them by using them, and relogs\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the recipe shop tests are skipped\n");
        return;
    }
    Harness h(rso_character(rso::kScribe, {}, {}), true, true, ws_real_db());
    CHECK(h.s != nullptr, "(joined)");
    if (!h.s) return;
    const TlsVendor lab = tls_add_vendor(h, rso::kLaboratoryAsset, "Laboratory Vendor", 400, 0);
    CHECK(lab.id != 0, "the Laboratory vendor stands 4 m from the player (its asset's link)");
    if (lab.id == 0) return;
    ws_phase_in(h);

    // the window: ClientOpenVendor names the Laboratory vendor's def and the shopkeeper, the list of hidden listings is empty, and the taxes are answered
    size_t from = h.got.size();
    h.request(ws_interact(lab.component), h.pawn_ch);
    std::vector<repl::RpcCall> calls = rso_calls_since(h, from);
    CHECK(h.s->vendor_opens() == 1 && h.s->vendor_def() == rso::kLaboratory && h.s->vendor_actor() == lab.id, "using the Laboratory vendor opens its window");
    CHECK(rso_count(calls, "ClientOpenVendor") == 1 && rso_opens(calls, rso::kLaboratory, lab.guid), "ClientOpenVendor names the Laboratory vendor's def and the shopkeeper");
    const auto lists = tlr_open_lists(h.got, from, h.pc_ch);
    CHECK(lists.size() == 1 && lists[0].has_value() && lists[0]->empty(), "the window hides nothing: no tab and no listing is on the unavailable list");
    const world::CityNodeInfo* node = h.fx.world->city_nodes() ? h.fx.world->city_nodes()->by_name("Halcyon") : nullptr;
    CHECK(node != nullptr, "(the world knows a node to ask the taxes of)");
    if (!node) return;
    from = h.got.size();
    h.request(rso_tax_request(node->guid), h.pc_ch);
    calls = rso_calls_since(h, from);
    CHECK(h.s->tax_requests() == 1 && h.s->tax_tables_sent() == 1 && rso_count(calls, "ClientSetNodeTaxTable") == 1, "the window's ServerGetNodeTaxTable is answered with one ClientSetNodeTaxTable");

    // the buy: 43 copper and 22 of tax, the item in the item bag, the cost and the Add shown to the client
    from = h.got.size();
    h.request(tls_purchase(tls_cart(rso::kLaboratory, {{rso::kIntelligenceItem, 1}})), h.pc_ch);
    calls = rso_calls_since(h, from);
    CHECK(h.s->vendor_purchases() == 1 && h.s->vendor_refused() == 0, "the cart of the Scroll of Intelligence recipe item is applied");
    CHECK(h.s->wallet().copper() == 100000 - rso::kRecipeItemCost, "the purse paid 65 copper: 43 and the node's tax of 22");
    const auto item = tls_in_bag(*h.s, rso::kIntelligenceItem);
    CHECK(item.has_value() && item->quantity == 1, "the recipe item is in the item bag");
    CHECK(rso_count(calls, "ClientShowCurrencyCallout") == 1 && !ws_storage_events_since(h, from).empty(), "the client was shown the cost and the item that arrived");
    if (!item) return;

    // using it teaches the recipe (a Scribe at Novice holds the certification), and the item is used up
    h.request(item_used(3, item->slot, item->instance_guid), h.pawn_ch);
    CHECK(std::find(h.s->taught_recipes().begin(), h.s->taught_recipes().end(), rso::kIntelligenceRecipe) != h.s->taught_recipes().end(), "using the recipe item teaches Scribe_T1_Scroll_Intelligence");
    CHECK(!tls_in_bag(*h.s, rso::kIntelligenceItem).has_value(), "the recipe item is used up");

    // the second recipe item and the two reagents of the Scribe tab, in one cart: 65 + 3 x 51 + 2 x 12
    h.request(tls_purchase(tls_cart(rso::kLaboratory, {{rso::kStrengthItem, 1}, {rso::kCrudePaper, 3}, {rso::kEnchantingEssence, 2}})), h.pc_ch);
    CHECK(h.s->vendor_purchases() == 2 && h.s->wallet().copper() == 100000 - rso::kRecipeItemCost - (rso::kRecipeItemCost + 3 * rso::kCrudePaperCost + 2 * rso::kReagentCost),
          "a cart of the Strength recipe item, 3 crude paper (34 copper each) and 2 enchanting essence (8 each) costs 65 + 153 + 24");
    CHECK(tls_total(*h.s, rso::kCrudePaper) == 3 && tls_total(*h.s, rso::kEnchantingEssence) == 2 && tls_in_bag(*h.s, rso::kStrengthItem).has_value(), "the reagents are in the material bag and the recipe item in the item bag");
    if (const auto strength = tls_in_bag(*h.s, rso::kStrengthItem)) {
        h.request(item_used(3, strength->slot, strength->instance_guid), h.pawn_ch);
        CHECK(std::find(h.s->taught_recipes().begin(), h.s->taught_recipes().end(), rso::kStrengthRecipe) != h.s->taught_recipes().end(), "using it teaches Scribe_T1_Scroll_Strength");
    }

    // a known recipe's item is still sold (the shop checks no knowledge: the check stays on use), and its use is refused with the item kept
    const int64_t purse = h.s->wallet().copper();
    h.request(tls_purchase(tls_cart(rso::kLaboratory, {{rso::kIntelligenceItem, 1}})), h.pc_ch);
    CHECK(h.s->vendor_purchases() == 3 && h.s->wallet().copper() == purse - rso::kRecipeItemCost, "the shop sells the recipe item again to a Scribe who knows the recipe: 65 copper");
    if (const auto again = tls_in_bag(*h.s, rso::kIntelligenceItem)) {
        const size_t mark = static_cast<size_t>(g_log.tellp());
        h.request(item_used(3, again->slot, again->instance_guid), h.pawn_ch);
        CHECK(tls_in_bag(*h.s, rso::kIntelligenceItem).has_value() && g_log.str().find("is known already", mark) != std::string::npos, "using it again is refused, the item kept, the log says the recipe is known already");
    }

    // the relog: the session ends, what it saved is what a new session of the character starts with
    const int64_t paid_purse = h.s->wallet().copper();
    const std::vector<int64_t> taught = h.s->taught_recipes();
    h.s->finish("the connection was lost");
    CHECK(!h.sink.inventories.empty() && h.s->wallet().copper() == paid_purse, "the end of the session hands the bags over, and the purse is as it was");
    const std::vector<svc::InventoryItem> rows = h.sink.inventories.empty() ? std::vector<svc::InventoryItem>{} : h.sink.inventories.back().items;
    int64_t crude = 0, essence = 0, recipe_item = 0;
    for (const svc::InventoryItem& it : rows) {
        crude += it.item_record_id == rso::kCrudePaper ? it.quantity : 0;
        essence += it.item_record_id == rso::kEnchantingEssence ? it.quantity : 0;
        recipe_item += it.item_record_id == rso::kIntelligenceItem ? it.quantity : 0;
    }
    CHECK(crude == 3 && essence == 2 && recipe_item == 1, "the saved bags hold the 3 crude paper, the 2 essence and the unused recipe item");
    Harness again(rso_character(rso::kScribe, taught, rows, paid_purse), true, true, ws_real_db());
    CHECK(again.s != nullptr && again.s->wallet().copper() == paid_purse && tls_total(*again.s, rso::kCrudePaper) == 3 && tls_in_bag(*again.s, rso::kIntelligenceItem).has_value(),
          "the new session has the items and the reduced purse");
    CHECK(again.s != nullptr && again.s->taught_recipes() == taught && again.s->taught_recipes().size() == 2, "and the two recipes it learned");
}
AOC_REGISTER_TEST(test_rso_laboratory_buy_learn_relog);

// ---- who may use a recipe item ---------------------------------------------------------------------------------------------

void test_rso_recipe_item_use_needs_novice_only() {
    std::printf("recipe shops 1: a recipe item is sold to anyone, and its use asks for the Novice certification, which every character holds in every profession\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the recipe shop tests are skipped\n");
        return;
    }
    // a Jeweler (certified at Novice in Jeweler alone, as every character is in every other profession) buys a Scribe recipe item (no check at the shop) and uses it: the five items ask for
    // the Novice certification of their profession, and a character holds Novice in all 22 professions from the start, so the use check refuses none of them but a second use of a known recipe
    Harness h(rso_character(rso::kJeweler, {}, {}), true, true, ws_real_db());
    CHECK(h.s != nullptr, "(joined)");
    if (!h.s) return;
    const TlsVendor lab = tls_add_vendor(h, rso::kLaboratoryAsset, "Laboratory Vendor", 400, 0);
    const TlsVendor stone = tls_add_vendor(h, rso::kStoneworksAsset, "Stoneworks Vendor", 0, 400);
    if (lab.id == 0 || stone.id == 0) return;
    ws_phase_in(h);
    CHECK(h.s->professions().can_use(rso::kScribe, 0, 1) && h.s->professions().can_use(rso::kJeweler, 0, 1), "a Jeweler holds the Novice certification of Scribe and of Jeweler alike");
    CHECK(rso_open_window(h, lab, rso::kLaboratory), "the Laboratory window opens for a Jeweler");
    h.request(tls_purchase(tls_cart(rso::kLaboratory, {{rso::kIntelligenceItem, 1}})), h.pc_ch);
    CHECK(h.s->vendor_purchases() == 1 && h.s->wallet().copper() == 100000 - rso::kRecipeItemCost, "the shop sells it to a Jeweler (65 copper): it asks for no certification");
    const auto item = tls_in_bag(*h.s, rso::kIntelligenceItem);
    CHECK(item.has_value(), "the Scribe recipe item is in the Jeweler's bag");
    if (!item) return;
    h.request(item_used(3, item->slot, item->instance_guid), h.pawn_ch);
    CHECK(std::find(h.s->taught_recipes().begin(), h.s->taught_recipes().end(), rso::kIntelligenceRecipe) != h.s->taught_recipes().end() && !tls_in_bag(*h.s, rso::kIntelligenceItem).has_value(),
          "using it teaches Scribe_T1_Scroll_Intelligence and uses the item up: the Novice certification is all it asks for");

    // the Stoneworks window of the same character, and the Jeweler's own recipe item is used
    CHECK(rso_open_window(h, stone, rso::kStoneworks) && h.s->vendor_opens() == 2, "the Stoneworks window opens next (a second window replaces the first)");
    h.request(tls_purchase(tls_cart(rso::kStoneworks, {{rso::kRingItem, 1}})), h.pc_ch);
    const auto ring = tls_in_bag(*h.s, rso::kRingItem);
    CHECK(h.s->vendor_purchases() == 2 && ring.has_value(), "the Ring recipe item is bought at the Stoneworks vendor");
    if (!ring) return;
    h.request(item_used(3, ring->slot, ring->instance_guid), h.pawn_ch);
    CHECK(std::find(h.s->taught_recipes().begin(), h.s->taught_recipes().end(), rso::kRingRecipe) != h.s->taught_recipes().end() && !tls_in_bag(*h.s, rso::kRingItem).has_value(),
          "the Jeweler learns Jeweler_T1_Ring_Flowmarked, and the item is used up");
}
AOC_REGISTER_TEST(test_rso_recipe_item_use_needs_novice_only);

// ---- the Stoneworks vendor ---------------------------------------------------------------------------------------------------

void test_rso_stoneworks_buy_learn() {
    std::printf("recipe shops 1: at the Stoneworks vendor (its instance's link) a Jeweler buys the three Flowmarked recipe items and learns them\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the recipe shop tests are skipped\n");
        return;
    }
    Harness h(rso_character(rso::kJeweler, {}, {}), true, true, ws_real_db());
    CHECK(h.s != nullptr, "(joined)");
    if (!h.s) return;
    const TlsVendor stone = tls_add_vendor(h, 0, "Stoneworks Vendor", 400, 0);   // no asset: only the instance's link opens the shop
    CHECK(stone.id != 0, "the Stoneworks vendor stands 4 m from the player");
    if (stone.id == 0) return;
    ws_phase_in(h);
    h.request(ws_interact(stone.component), h.pawn_ch);
    CHECK(h.s->vendor_opens() == 0 && h.s->interactions_ignored() == 1, "a shopkeeper with no link does not open a window");
    rso_link_by_instance(h, stone, rso::kStoneworksInstance);
    const size_t from = h.got.size();
    h.request(ws_interact(stone.component), h.pawn_ch);
    std::vector<repl::RpcCall> calls = rso_calls_since(h, from);
    CHECK(h.s->vendor_opens() == 1 && h.s->vendor_def() == rso::kStoneworks, "with the instance's link of the data it opens the Stoneworks vendor's window");
    CHECK(rso_count(calls, "ClientOpenVendor") == 1 && rso_opens(calls, rso::kStoneworks, stone.guid), "ClientOpenVendor names the Stoneworks vendor's def and the shopkeeper");
    const auto lists = tlr_open_lists(h.got, from, h.pc_ch);
    CHECK(lists.size() == 1 && lists[0].has_value() && lists[0]->empty(), "the window hides nothing");
    const world::CityNodeInfo* node = h.fx.world->city_nodes() ? h.fx.world->city_nodes()->by_name("Halcyon") : nullptr;
    if (!node) return;
    const size_t taxes = h.got.size();
    h.request(rso_tax_request(node->guid), h.pc_ch);
    CHECK(h.s->tax_tables_sent() == 1 && rso_count(rso_calls_since(h, taxes), "ClientSetNodeTaxTable") == 1, "the taxes are answered with one ClientSetNodeTaxTable");

    // the three recipe items in one cart: 3 x 65
    const size_t buying = h.got.size();
    h.request(tls_purchase(tls_cart(rso::kStoneworks, {{rso::kJewelerItems[0].item, 1}, {rso::kJewelerItems[1].item, 1}, {rso::kJewelerItems[2].item, 1}})), h.pc_ch);
    CHECK(h.s->vendor_purchases() == 1 && h.s->wallet().copper() == 100000 - 3 * rso::kRecipeItemCost, "the Earring, Necklace and Ring recipe items cost 3 x 65 copper");
    CHECK(rso_count(rso_calls_since(h, buying), "ClientShowCurrencyCallout") == 1 && !ws_storage_events_since(h, buying).empty(), "the client was shown the cost and the items");
    size_t learned = 0;
    for (const rso::RecipeItem& ri : rso::kJewelerItems) {
        const auto it = tls_in_bag(*h.s, ri.item);
        CHECK(it.has_value(), (std::string(ri.name) + " is in the item bag").c_str());
        if (!it) continue;
        h.request(item_used(3, it->slot, it->instance_guid), h.pawn_ch);
        learned += std::find(h.s->taught_recipes().begin(), h.s->taught_recipes().end(), ri.recipe) != h.s->taught_recipes().end() ? 1 : 0;
    }
    CHECK(learned == 3 && h.s->taught_recipes().size() == 3, "using the three teaches the three Flowmarked Jeweler recipes");
}
AOC_REGISTER_TEST(test_rso_stoneworks_buy_learn);

// ---- every tab is live ------------------------------------------------------------------------------------------------------

/// One cart of one of every listing of a tab of `shop`: bought whole, every item arrives, and the purse pays each price plus the tax.
static void rso_buy_whole_tab(int64_t shop, int64_t asset, size_t tab_index, const char* tab_name, size_t expected_listings) {
    Harness h(rso_character(rso::kScribe, {}, {}), true, true, ws_real_db());
    if (!h.s) {
        std::printf("  [FAIL] the session does not join\n");
        ++g_fail;
        return;
    }
    const TlsVendor v = tls_add_vendor(h, asset, "Shopkeeper", 400, 0);
    const vendor::TownShops* shops = h.fx.world->town_shops();
    const content::VendorData::Def* def = shops ? shops->def(shop) : nullptr;
    if (v.id == 0 || !def || tab_index >= def->inventories.size()) {
        std::printf("  [FAIL] the %s tab cannot be set up\n", tab_name);
        ++g_fail;
        return;
    }
    ws_phase_in(h);
    const bool opened = rso_open_window(h, v, shop);
    std::vector<std::pair<int, int64_t>> listings;   // (ord, item)
    int64_t expected = 0;
    for (const auto& [item, rows] : shops->data().prices.find(shop)->second) {
        for (const content::VendorPriceRow& r : rows) {
            if (r.inventory != def->inventories[tab_index]) continue;
            listings.emplace_back(r.ord, item);
            expected += vendor::taxed_unit(r.price, shops->default_rate(shop));
        }
    }
    std::sort(listings.begin(), listings.end());
    repl::VendorPurchaseCart cart;
    cart.vendor = shop;
    for (const auto& [ord, item] : listings) cart.items.push_back({item, 1, std::nullopt, std::nullopt});
    const size_t mark = static_cast<size_t>(g_log.tellp());
    h.request(tls_purchase(cart), h.pc_ch);
    size_t arrived = 0;
    for (const auto& [ord, item] : listings) arrived += tls_total(*h.s, item) == 1 ? 1 : 0;
    const std::string label = std::string("the ") + tab_name + " tab: ";
    CHECK(opened && listings.size() == expected_listings, (label + "its window opens and it has " + std::to_string(expected_listings) + " listings").c_str());
    if (h.s->vendor_purchases() != 1) std::printf("    (the log: %s)\n", g_log.str().substr(mark).c_str());
    CHECK(h.s->vendor_purchases() == 1 && h.s->vendor_refused() == 0, (label + "one cart of every listing is bought whole").c_str());
    CHECK(arrived == listings.size(), (label + "every listing's item is in the character's bags").c_str());
    CHECK(h.s->wallet().copper() == 100000 - expected, (label + "the purse paid each price plus the node's tax (" + std::to_string(expected) + " copper)").c_str());
}

void test_rso_every_tab_sells() {
    std::printf("recipe shops 1: every tab of the two shops sells, one cart of each listing\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the recipe shop tests are skipped\n");
        return;
    }
    rso_buy_whole_tab(rso::kLaboratory, rso::kLaboratoryAsset, 0, "Laboratory Alchemy", 16);
    rso_buy_whole_tab(rso::kLaboratory, rso::kLaboratoryAsset, 1, "Laboratory Arcane Engineering", 15);
    rso_buy_whole_tab(rso::kLaboratory, rso::kLaboratoryAsset, 2, "Laboratory Scribe", 9);
    rso_buy_whole_tab(rso::kStoneworks, rso::kStoneworksAsset, 0, "Stoneworks Jeweler", 8);
    rso_buy_whole_tab(rso::kStoneworks, rso::kStoneworksAsset, 1, "Stoneworks Mining", 1);
    rso_buy_whole_tab(rso::kStoneworks, rso::kStoneworksAsset, 2, "Stoneworks Stonemasonry", 5);
}
AOC_REGISTER_TEST(test_rso_every_tab_sells);

// ---- the refusals -----------------------------------------------------------------------------------------------------------

void test_rso_range_refusals() {
    std::printf("recipe shops 1: the shopkeeper must be within 12 m to open the window and to sell\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the recipe shop tests are skipped\n");
        return;
    }
    // 13 m away: the window does not open, and a cart sent anyway is refused (no window is open)
    {
        Harness h(rso_character(rso::kScribe, {}, {}), true, true, ws_real_db());
        if (!h.s) return;
        const TlsVendor distant = tls_add_vendor(h, rso::kLaboratoryAsset, "Laboratory Vendor", 1300, 0);
        if (distant.id == 0) return;
        ws_phase_in(h);
        const size_t mark = static_cast<size_t>(g_log.tellp());
        h.request(ws_interact(distant.component), h.pawn_ch);
        CHECK(h.s->vendor_opens() == 0 && h.s->vendor_def() == 0 && h.s->interactions_ignored() == 1, "at 13 m the Laboratory vendor's window does not open");
        CHECK(g_log.str().find("13 m away (a shop opens within 12 m)", mark) != std::string::npos, "the log says the shopkeeper is 13 m away and a shop opens within 12 m");
        h.request(tls_purchase(tls_cart(rso::kLaboratory, {{rso::kIntelligenceItem, 1}})), h.pc_ch);
        CHECK(h.s->vendor_purchases() == 0 && h.s->vendor_refused() == 1 && h.s->wallet().copper() == 100000 && !tls_held(*h.s, rso::kIntelligenceItem).has_value(), "a cart sent with no window open is refused: nothing paid, nothing given");
    }
    // 11.9 m: inside the range
    {
        Harness h(rso_character(rso::kScribe, {}, {}), true, true, ws_real_db());
        if (!h.s) return;
        const TlsVendor inside = tls_add_vendor(h, rso::kLaboratoryAsset, "Laboratory Vendor", 1190, 0);
        if (inside.id == 0) return;
        ws_phase_in(h);
        CHECK(rso_open_window(h, inside, rso::kLaboratory), "at 11.9 m the window opens");
    }
    // the window is open, then the shopkeeper is 13 m away: the cart is refused and the window is closed; it needs a new use to open again
    {
        Harness h(rso_character(rso::kScribe, {}, {}), true, true, ws_real_db());
        if (!h.s) return;
        const TlsVendor v = tls_add_vendor(h, rso::kLaboratoryAsset, "Laboratory Vendor", 400, 0);
        if (v.id == 0) return;
        ws_phase_in(h);
        CHECK(rso_open_window(h, v, rso::kLaboratory), "the window opens at 4 m");
        h.fx.world->registry().modify(v.id, world::Change::Movement, [](world::WorldActor& w) { w.x = bootstrap::kDefaultSpawn.x + 1300; });
        rso_expect_refused(h, tls_cart(rso::kLaboratory, {{rso::kIntelligenceItem, 1}}), "13 m away", "a cart when the shopkeeper is 13 m away");
        CHECK(h.s->vendor_def() == 0, "the window is closed by the refusal");
        h.fx.world->registry().modify(v.id, world::Change::Movement, [](world::WorldActor& w) { w.x = bootstrap::kDefaultSpawn.x + 400; });
        rso_expect_refused(h, tls_cart(rso::kLaboratory, {{rso::kIntelligenceItem, 1}}), "no vendor window is open", "a cart when the shopkeeper is back at 4 m but the window was not reopened");
        CHECK(rso_open_window(h, v, rso::kLaboratory), "using the shopkeeper again opens the window");
        h.request(tls_purchase(tls_cart(rso::kLaboratory, {{rso::kIntelligenceItem, 1}})), h.pc_ch);
        CHECK(h.s->vendor_purchases() == 1 && h.s->wallet().copper() == 100000 - rso::kRecipeItemCost, "and the same cart is bought");
    }
}
AOC_REGISTER_TEST(test_rso_range_refusals);

void test_rso_cart_refusals() {
    std::printf("recipe shops 1: a cart that is not a listing of the open shop, has a bad quantity or is empty is refused whole\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the recipe shop tests are skipped\n");
        return;
    }
    Harness h(rso_character(rso::kScribe, {}, {}), true, true, ws_real_db());
    if (!h.s) return;
    const TlsVendor lab = tls_add_vendor(h, rso::kLaboratoryAsset, "Laboratory Vendor", 400, 0);
    if (lab.id == 0) return;
    ws_phase_in(h);
    CHECK(rso_open_window(h, lab, rso::kLaboratory), "the Laboratory window is open");
    const size_t bag_items = h.s->inventory().items().size();
    // a listing of another shop: the Textile Mill's recipe item (named in its own cart and in the Laboratory's), and the Stoneworks vendor's Ring
    rso_expect_refused(h, tls_cart(rso::kLaboratory, {{tls::kBackTeaching, 1}}), "is not one of ArtisanshipPOI_Laboratory_Vendor's listings", "a Textile Mill listing in a Laboratory cart");
    rso_expect_refused(h, tls_cart(tls::kTextileMill, {{tls::kBackTeaching, 1}}), "the cart names vendor", "a Textile Mill cart at the Laboratory");
    rso_expect_refused(h, tls_cart(rso::kLaboratory, {{rso::kRingItem, 1}}), "is not one of ArtisanshipPOI_Laboratory_Vendor's listings", "the Stoneworks vendor's Ring recipe item in a Laboratory cart");
    rso_expect_refused(h, tls_cart(rso::kStoneworks, {{rso::kRingItem, 1}}), "the cart names vendor", "a Stoneworks cart at the Laboratory");
    // a good line next to a bad one: the whole cart goes
    rso_expect_refused(h, tls_cart(rso::kLaboratory, {{rso::kIntelligenceItem, 1}, {tls::kBackTeaching, 1}}), "is not one of ArtisanshipPOI_Laboratory_Vendor's listings", "a good line next to a Textile Mill line");
    // the quantity: 0 and 100,001
    rso_expect_refused(h, tls_cart(rso::kLaboratory, {{rso::kIntelligenceItem, 0}}), "asks for a quantity of 0", "a quantity of 0");
    rso_expect_refused(h, tls_cart(rso::kLaboratory, {{rso::kWater, 100001}}), "asks for a quantity of 100001", "a quantity of 100,001");
    // an empty cart (a cart of 71 lines is checked in the data test, through the shops' own quote: its 11.5 thousand bits are more than the test client's one-packet bunch holds, and the
    // client's partial bunches are not modelled here)
    rso_expect_refused(h, tls_cart(rso::kLaboratory, {}), "the cart is empty", "an empty cart");
    CHECK(h.s->vendor_purchases() == 0 && h.s->wallet().copper() == 100000 && h.s->inventory().items().size() == bag_items, "(no refusal changed the purse or the bags)");
}
AOC_REGISTER_TEST(test_rso_cart_refusals);

void test_rso_purse_bag_and_double_submit() {
    std::printf("recipe shops 1: a purse 1 copper short, a full bag and two carts at once: refused or paid whole\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the recipe shop tests are skipped\n");
        return;
    }
    // 64 copper: one short of 65
    {
        Harness h(rso_character(rso::kScribe, {}, {}, rso::kRecipeItemCost - 1), true, true, ws_real_db());
        if (!h.s) return;
        const TlsVendor lab = tls_add_vendor(h, rso::kLaboratoryAsset, "Laboratory Vendor", 400, 0);
        if (lab.id == 0) return;
        ws_phase_in(h);
        CHECK(rso_open_window(h, lab, rso::kLaboratory), "the window opens for a purse of 64 copper");
        rso_expect_refused(h, tls_cart(rso::kLaboratory, {{rso::kIntelligenceItem, 1}}), "(not enough money)", "a purse 1 copper short");
        CHECK(h.s->wallet().copper() == rso::kRecipeItemCost - 1, "the purse is 64 still");
    }
    // 65 copper: exactly enough, and the purse is empty after
    {
        Harness h(rso_character(rso::kScribe, {}, {}, rso::kRecipeItemCost), true, true, ws_real_db());
        if (!h.s) return;
        const TlsVendor lab = tls_add_vendor(h, rso::kLaboratoryAsset, "Laboratory Vendor", 400, 0);
        if (lab.id == 0) return;
        ws_phase_in(h);
        CHECK(rso_open_window(h, lab, rso::kLaboratory), "the window opens for a purse of 65 copper");
        h.request(tls_purchase(tls_cart(rso::kLaboratory, {{rso::kIntelligenceItem, 1}})), h.pc_ch);
        CHECK(h.s->vendor_purchases() == 1 && h.s->wallet().copper() == 0 && tls_in_bag(*h.s, rso::kIntelligenceItem).has_value(), "65 copper is enough: the purse is 0 and the item is in the bag");
    }
    // a full item bag: the recipe item has no slot, so the cart is refused whole (a reagent, which goes to the material bag, would still fit)
    {
        std::vector<svc::InventoryItem> full;
        for (int i = 0; i < items::kBackpackSlots; ++i) full.push_back(tls_stack(i, tls::kBackItem, 1));
        Harness h(rso_character(rso::kScribe, {}, std::move(full)), true, true, ws_real_db());
        if (!h.s) return;
        const TlsVendor lab = tls_add_vendor(h, rso::kLaboratoryAsset, "Laboratory Vendor", 400, 0);
        if (lab.id == 0) return;
        ws_phase_in(h);
        CHECK(h.s->inventory().count(svc::ItemContainer::Backpack) == static_cast<size_t>(items::kBackpackSlots), "(a full item bag)");
        CHECK(rso_open_window(h, lab, rso::kLaboratory), "the window opens");
        rso_expect_refused(h, tls_cart(rso::kLaboratory, {{rso::kEnchantingEssence, 2}, {rso::kIntelligenceItem, 1}}), "room", "a full bag, a reagent and a recipe item in one cart");
        CHECK(!tls_held(*h.s, rso::kEnchantingEssence).has_value(), "the reagent that would have fitted was not given either");
    }
    // two carts of the same recipe item in one datagram, with the money for one and a half: one is bought and the other refused
    {
        Harness h(rso_character(rso::kScribe, {}, {}, 100), true, true, ws_real_db());
        if (!h.s) return;
        const TlsVendor lab = tls_add_vendor(h, rso::kLaboratoryAsset, "Laboratory Vendor", 400, 0);
        if (lab.id == 0) return;
        ws_phase_in(h);
        CHECK(rso_open_window(h, lab, rso::kLaboratory), "the window opens for a purse of 100 copper");
        const repl::VendorPurchaseCart cart = tls_cart(rso::kLaboratory, {{rso::kIntelligenceItem, 1}});
        h.client_sends({h.client.actor(h.pc_ch, tls_purchase(cart), true), h.client.actor(h.pc_ch, tls_purchase(cart), true)});
        h.settle();
        const auto item = tls_in_bag(*h.s, rso::kIntelligenceItem);
        CHECK(h.s->vendor_purchases() == 1 && h.s->vendor_refused() == 1 && h.s->wallet().copper() == 100 - rso::kRecipeItemCost && item.has_value() && item->quantity == 1,
              "two carts in one datagram: one is bought (65 copper, one item) and the other refused (not enough money)");
    }
}
AOC_REGISTER_TEST(test_rso_purse_bag_and_double_submit);

// ---- what stays closed -------------------------------------------------------------------------------------------------------

void test_rso_other_shops_stay_closed() {
    std::printf("recipe shops 1: the Barracks, the citizen outfitter, the mayor's shop and the artisan buildings' vendors still say they have nothing to offer\n");
    if (!ws_real_db()) {
        std::printf("  [skip] no real content.db: the recipe shop tests are skipped\n");
        return;
    }
    Harness h(rso_character(rso::kScribe, {}, {}), true, true, ws_real_db());
    if (!h.s) return;
    struct Keeper {
        const char* name;
        int64_t     asset, instance;
    };
    const Keeper keepers[] = {{"Barracks vendor (instance link)", 0, 6064632786098585602ll},
                              {"citizen outfitter (asset link)", 6064632384032473093ll, 0},
                              {"mayor's shop (asset link)", 6064632384035356678ll, 0},
                              {"Laboratory building's Scribe vendor (asset link)", 6064632309238464559ll, 0},
                              {"Stoneworks building's Jeweler vendor (asset link)", 6064632309283618868ll, 0}};
    double dx = 300;
    std::vector<std::pair<const Keeper*, TlsVendor>> placed;
    for (const Keeper& k : keepers) {
        const TlsVendor v = tls_add_vendor(h, k.asset, k.name, dx, 0);
        if (v.id == 0) return;
        if (k.instance != 0) rso_link_by_instance(h, v, k.instance);
        placed.emplace_back(&k, v);
        dx += 100;
    }
    ws_phase_in(h);
    for (const auto& [k, v] : placed) {
        const size_t mark = static_cast<size_t>(g_log.tellp());
        h.request(ws_interact(v.component), h.pawn_ch);
        CHECK(h.s->vendor_opens() == 0 && g_log.str().find("has nothing to offer yet", mark) != std::string::npos, (std::string("the ") + k->name + " opens no window and the log says it has nothing to offer yet").c_str());
    }
    CHECK(h.s->vendor_opens() == 0 && h.s->interactions_ignored() == placed.size(), "none of the five opened a window");
}
AOC_REGISTER_TEST(test_rso_other_shops_stay_closed);
