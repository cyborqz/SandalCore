// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  weaponsmithing_review_tests.inc: where the tool recipes' reagents come from (the Weaponsmithing milestone, after its review)
//
//  The 54 tool recipes take a reagent each (the pickaxe, sickle, hunting bow and fishing pole ones are the reagents of 46 of them).  The data sells all four for 8 copper at the
//  two vendors of the artisan outposts the Riverlands start in (Aven's End and Briarmoor Workshops: the Smithy vendor and the Woodshop vendor), whose NPCs the world spawns
//  at the outposts' own plots; with their shops switched off the reagents had no source in the live world, and the Novice tools and the Apprentice Hunting Bow waited on a game
//  master's give-item.  Included by test_worldd_professions.cpp after weaponsmithing_tests.inc, before main().
// ============================================================================
#include "vendor/town_shops.h"

namespace wsr {
constexpr int64_t kPickAxeReagent = 6064634673790713877ll;      // Resource_Reagent_Artisan_PickAxe
constexpr int64_t kSickleReagent = 6064634673790910486ll;       // Resource_Reagent_Artisan_Sickle
constexpr int64_t kHuntingBowReagent = 6064634673789665297ll;   // Resource_Reagent_Artisan_HuntingBow
constexpr int64_t kFishingPoleReagent = 6064634673790189587ll;  // Resource_Reagent_Artisan_FIshingPole
constexpr int64_t kSmithyVendor = 6064630643960643621ll;        // ArtisanshipPOI_Smithy_Vendor
constexpr int64_t kWoodshopVendor = 6064630643964837928ll;      // ArtisanshipPOI_Woodshop_Vendor
}  // namespace wsr

/// The two outpost shops sell the tool reagents, they are open, every NPC the data links to them opens them, and a cart of a reagent is priced.
static void test_ws_reagent_shops(const content::ContentDb& db) {
    std::printf("the tool reagents: the artisan outposts' Smithy and Woodshop vendors sell them and their shops are open\n");
    std::string error;
    auto data = db.vendor_data(error);
    if (!data) {
        std::printf("  [FAIL] the shops do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    const vendor::TownShops shops(std::move(*data));
    const content::VendorData::Def* smithy = shops.def(wsr::kSmithyVendor);
    const content::VendorData::Def* woodshop = shops.def(wsr::kWoodshopVendor);
    CHECK_SHOWN(smithy && smithy->name == "ArtisanshipPOI_Smithy_Vendor" && woodshop && woodshop->name == "ArtisanshipPOI_Woodshop_Vendor",
                "the Smithy vendor and the Woodshop vendor of the artisan outposts are in the data");
    if (!smithy || !woodshop) return;
    CHECK_SHOWN(shops.is_open(wsr::kSmithyVendor) && shops.is_open(wsr::kWoodshopVendor), "both shops are switched on");

    // every NPC the data links to them opens them (instance links first, the asset's otherwise): the outposts' vendors at Aven's End and Briarmoor Workshops
    size_t instances = 0, assets = 0, opening = 0;
    for (const auto& [instance, shop] : shops.data().instance_links) {
        if (shop != wsr::kSmithyVendor && shop != wsr::kWoodshopVendor) continue;
        ++instances;
        opening += shops.shop_of(instance, 0) == shop ? 1 : 0;
    }
    for (const auto& [asset, shop] : shops.data().asset_links) {
        if (shop != wsr::kSmithyVendor && shop != wsr::kWoodshopVendor) continue;
        ++assets;
        opening += shops.shop_of(0, asset) == shop ? 1 : 0;
    }
    CHECK_SHOWN(instances >= 2 && assets == 2 && opening == instances + assets, "every NPC instance and asset linked to the two vendors opens its shop");

    // what they sell: the reagents at 8 copper a unit, one listing each, no variation and no offer
    const struct {
        int64_t shop;
        int64_t item;
        const char* name;
    } reagents[] = {{wsr::kSmithyVendor, wsr::kPickAxeReagent, "pickaxe"},       {wsr::kSmithyVendor, wsr::kSickleReagent, "sickle"},
                    {wsr::kWoodshopVendor, wsr::kHuntingBowReagent, "hunting bow"}, {wsr::kWoodshopVendor, wsr::kFishingPoleReagent, "fishing pole"}};
    const vendor::TownShops::Holds holds = [](const content::VendorPredicateRow&) { return true; };
    for (const auto& r : reagents) {
        const auto prices = shops.data().prices.find(r.shop);
        bool listed = false, eight = false;
        if (prices != shops.data().prices.end()) {
            const auto item = prices->second.find(r.item);
            if (item != prices->second.end()) {
                listed = item->second.size() == 1 && item->second.front().variation == 0 && item->second.front().offer == 0;
                eight = listed && item->second.front().price == 8 && item->second.front().currency == items::kMoneyTier;
            }
        }
        CHECK_SHOWN(listed && eight, (std::string("the shop lists the ") + r.name + " reagent once, for 8 copper").c_str());
        repl::VendorPurchaseCart cart;
        cart.vendor = r.shop;
        repl::VendorCartItem line;
        line.item_record = r.item;
        line.quantity = 8;
        cart.items.push_back(line);
        const vendor::TownShops::Quote quote = shops.quote(r.shop, cart, shops.default_rate(r.shop), holds);
        const int64_t unit = vendor::taxed_unit(8, shops.default_rate(r.shop));
        CHECK_SHOWN(quote.ok && quote.lines.size() == 1 && quote.lines.front().unit == unit && quote.total == unit * 8 && quote.currency == items::kMoneyTier,
                    (std::string("a cart of 8 ") + r.name + " reagents is priced with the node's tax, in copper").c_str());
    }
    {
        const vendor::TaxRate rate = shops.default_rate(wsr::kSmithyVendor);
        std::printf("    (a reagent costs %lld copper with the default node tax)\n", static_cast<long long>(vendor::taxed_unit(8, rate)));
    }

    // the tool recipes: 46 of the 54 take one of these four reagents, so an open shop sells their reagent; the 8 others take none (the Golgora, the Master and Grand Master pickaxes and
    // sickles) or one no shop of the live world sells (the Jenry, first-time-player and retired quest items)
    std::string recipe_error;
    auto recipes = db.craft_recipe_data(recipe_error);
    CHECK_SHOWN(recipes.has_value(), "the recipes read");
    if (!recipes) return;
    std::set<int64_t> sold;   // every item an open shop lists
    for (const int64_t shop : shops.open_shops()) {
        const auto prices = shops.data().prices.find(shop);
        if (prices == shops.data().prices.end()) continue;
        for (const auto& priced : prices->second) sold.insert(priced.first);
    }
    size_t tools = 0, fed = 0;
    for (const auto& [guid, r] : recipes->recipes) {
        if (r.inventory_destination) continue;
        ++tools;
        for (const content::CraftInput& in : r.inputs) {
            if (in.primary && in.item != 0 && sold.count(in.item) != 0) {
                ++fed;
                break;
            }
        }
    }
    CHECK_SHOWN(tools == 54 && fed == 46, "46 of the 54 tool recipes take a reagent that an open shop sells");
    SAYOK("the tool reagents' shops");
}

/// The Apprentice Hunting Bow, start to product: Carpentry - Apprentice, a taught Apprentice Carpenter, 15 Boards and 8 hunting bow reagents; what it costs, makes and pays.
static void test_ws_bow_craft(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("the Apprentice Hunting Bow at Carpentry - Apprentice: its price, product, quality, durability and experience\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    std::string station_error;
    const auto stations = db.craft_station_data(station_error);
    CHECK_SHOWN(loaded.has_value() && stations.has_value(), "the recipes and the stations read");
    if (!loaded || !stations) return;
    const content::CraftRecipeData data = ws_with_filters(*loaded);
    const content::CraftStationDef* carpentry = station_named(*stations, "CraftingStation_Carpentry_T2");
    const content::CraftRecipe* bow = data.recipe(ws::kBowRecipe[0]);
    CHECK_SHOWN(carpentry != nullptr && bow != nullptr && bow->name == "Carpentry_T2_Tool_Bow_0" && !bow->learnable && !bow->inventory_destination,
                "Carpentry_T2_Tool_Bow_0 is an Equipment recipe that needs no learning");
    if (!carpentry || !bow) return;
    const WsStart start = ws_start_for(data, *bow, 1, world);
    auto bag = bag_of(start.items);
    CHECK_SHOWN(bag.has_value() && start.req.tags.size() == 1, "a bag of 15 Boards (named by the filter's tags) and 8 hunting bow reagents");
    if (!bag) return;
    const cr::Plan plan = cr::plan_start(start.req, ws_facts(data, *carpentry, *bag, world, 1, false, 3));
    CHECK_SHOWN(plan.ok, "an Apprentice who was taught nothing starts it at Carpentry - Apprentice");
    if (!plan.ok) {
        std::printf("    %s\n", plan.refusal.c_str());
        return;
    }
    CHECK_SHOWN(plan.unit_cost == 1000 && plan.cost == plan.unit_taxed && plan.unit_taxed > plan.unit_cost, "it costs 1,000 copper and the station's service tax on top");
    CHECK_SHOWN(plan.takes.size() == 2 && plan.takes.front().lowest_first && plan.takes.front().count == 8, "the 8 reagents are taken first, then the Boards");
    const auto made = cr::roll_craft(data, *plan.recipe, ws_paid(plan), plan.ratings, 1, 99, [&](int64_t item) { return ws_sale(db, item); });
    CHECK_SHOWN(made.has_value() && made->products.size() == 1 && made->products.front().quantity == 1, "the quick make makes one bow");
    if (!made) return;
    std::string item_error;
    const auto item = db.item(made->products.front().item, item_error);
    CHECK_SHOWN(item.has_value() && item->name == "Gear_Artisan_Hunting_Tool_Bow_T2_0" && item->equip_slots == std::vector<uint8_t>{items::equip::Bow},
                "it is Gear_Artisan_Hunting_Tool_Bow_T2_0, worn in the Bow slot 38");
    if (!item) return;
    std::printf("    (the bow: quality %d, rarity %d, durability %.1f, %lld copper, Carpentry +%lld XP, Adventuring +%lld)\n", made->quality, made->rarity, item->max_durability_at(made->quality),
                static_cast<long long>(plan.cost), static_cast<long long>(made->profession_xp), static_cast<long long>(made->adventuring_xp));
    // 200 for the recipe's tier 1, times 1 to 6 for the rarity of the product (Common to Legendary), times 2 for a tool's slot
    CHECK_SHOWN(made->profession_xp == 200 * (made->rarity + 1) * 2, "it pays Carpentry experience by the formula: 200 for tier 1, x the rarity, x 2 for a tool");
    CHECK_SHOWN(made->adventuring_xp == 0, "and no Adventuring experience (the tool recipes' table names the harvest rule, which is worked out as none)");
    CHECK_SHOWN(item->max_durability_at(made->quality) > 0.0, "it has durability to wear");
    SAYOK("the Apprentice Hunting Bow's craft");
}
