// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tailoring_tests.inc: Tailoring, its two outpost vendors and the recipes they open (the Tailoring milestone)
//
//  The real content.db: 530 Tailoring recipes (119 Novice, 170 Apprentice, 241 Journeyman), 471 of them learned,
//  their station sets (119, 289 and 530) and their walls (296 name the Textile Mill's service building, 80 a node's
//  level, 25 a biome, 14 give the server no product).  The Textile Mill and Hideworks vendors of the artisan outposts
//  are switched on and their listings' GetNodeBiome condition is answered for the node the shop stands in: of their 39
//  Tailoring teaching items 33 (the six Emberwake pieces are listed for the Tabletop Mountains), the reagents and the
//  Weaving/Tanning reagents are for sale in the Riverlands, and the plan counts at the Novice, Apprentice and Journeyman
//  stations move with them.  What the stations and the shops then allow and refuse,
//  with the exact refusal text of each check; the live chain of the five recipes a fresh Novice can feed; and what is
//  held back (the Textile Mill's T3 station, the PTR recipe items, six reward tables that make nothing, the 26 siege
//  banners of the sieges phase).  Included by test_worldd_professions.cpp after its helpers, before main().
// ============================================================================
#include "vendor/gm_vendor.h"
#include "vendor/town_shops.h"
#include "world/city_nodes.h"

namespace tl {
constexpr int64_t kProfession = 108752746993277ll;

// the two outpost vendors
constexpr int64_t kTextileMill = 6064630643963592743ll, kHideworks = 6064630643946094627ll;

// the pieces of the cases
constexpr int64_t kBackRecipe = 6064630990264729600ll, kBackItem = 6064629897568846929ll;
constexpr int64_t kBackTeaching = 6064632254745673788ll;
constexpr int64_t kChestRecipe = 6064629387023482920ll, kChestItem = 109229507958396ll;
constexpr int64_t kScribeRecipe = 6064632236037243106ll, kScribeTeaching = 6064632270186152031ll;
constexpr int64_t kAnnalistRecipe = 6064631200434094179ll, kArcaneBackRecipe = 6064629387023482924ll;
constexpr int64_t kTorenChestRecipe = 6064632779703844910ll, kMiningShirtBefallen = 6064632717849001992ll;

// the inputs of the live chain
constexpr int64_t kThreadDaffodil = 6064631879482736645ll, kThreadSnowdrop = 6064631879480508420ll, kThreadCoarse = 6064634778922778975ll;
constexpr int64_t kSkinGrem = 6064630417661821030ll, kCarcassGrem = 6064630406144655379ll;
constexpr int64_t kLightChest = 6064634087927906604ll, kLightBack = 6064634087930659117ll, kLightFeet = 6064634087932887342ll, kLightLegs = 6064634087937147186ll;
constexpr int64_t kDrawtwineWax = 6064634007029809152ll, kBoltwrapTwine = 6064634007202955265ll;
constexpr int64_t kFleshloosenOil = 6064634002516934663ll, kHidecureLye = 6064634002700566536ll;
constexpr int64_t kArtisanBag = 6064634673791172631ll, kArtisanShirt = 6064634673789141007ll;
constexpr int64_t kPlantDaffodil = 6064629184431849472ll, kPlantSnowdrop = 6064629207102521358ll;
constexpr int64_t kOak = 109162169652924ll;
constexpr int64_t kPlantFilter = 6064633963143168007ll, kSailWoodFilter = 6064633963171938313ll;
constexpr int64_t kSiegeAithanahr = 6064632780487917574ll;
}  // namespace tl

/// One piece of a bag for the cases: a Backpack stack of `record` at `quality`.
static svc::InventoryItem tl_item(int slot, int64_t record, int64_t quantity, int quality = 1) {
    svc::InventoryItem it = farm_item(slot, record, static_cast<int>(quantity));
    it.quality = quality;
    return it;
}

/// The item a filter or tag input is paid with in these tests: one of the filter's items when the table lists them.
static int64_t tl_pick_for(const content::CraftRecipeData& data, const content::CraftInput& in) {
    namespace cr = aoc::worldd::crafting;
    if (in.filter != 0) {
        const auto it = data.filter_items.find(in.filter);
        if (it != data.filter_items.end() && !it->second.empty()) {
            const std::vector<std::string> tags = cr::tag_key(cr::split_tags(in.tags));
            for (const int64_t item : it->second) {
                bool all = true;
                for (const std::string& tag : tags) all = all && data.tag_admits(tag, item);
                if (all) return item;
            }
            return it->second.front();
        }
    }
    for (const std::string& key : cr::tag_key(cr::split_tags(in.tags))) {
        if (key == "item.resource.raw.plant") return tl::kPlantDaffodil;
        if (key == "item.resource.raw.wood") return tl::kOak;
        if (key == "item.resource.processed.thread") return tl::kThreadDaffodil;
    }
    return 0;
}

struct TlStart {
    repl::StartCraft                req;
    std::vector<svc::InventoryItem> items;
};

/// The bag and the request that start `recipe`: the reagent, each general input named at its stack's rarity, and a pick for
/// each input named by a filter (one of the filter's items when the table lists them), in a tag selection.
static TlStart tl_start_for(const content::CraftRecipeData& data, const content::CraftRecipe& recipe, int quantity, const PlayerWorld& world, int quality = 1) {
    namespace cr = aoc::worldd::crafting;
    TlStart s;
    s.req = start_of(recipe, quantity);
    int slot = 0;
    for (const content::CraftInput& in : recipe.inputs) {
        if (in.quantity < 1) continue;
        const int need = in.quantity * quantity;
        if (in.primary) {
            const int q = std::max(quality, quality_at_least(world, in.item, in.min_rarity));
            s.items.push_back(tl_item(slot++, in.item, need, q));
            continue;
        }
        if (in.item != 0) {
            const int q = std::max(quality, quality_at_least(world, in.item, in.min_rarity));
            s.items.push_back(tl_item(slot++, in.item, need, q));
            s.req.general.push_back(resource(in.item, need, world.item_rarity_facts(in.item).rarity_of(q)));
            continue;
        }
        const std::vector<std::string> tags = cr::split_tags(in.tags);
        const int64_t pick = tl_pick_for(data, in);
        if (pick == 0) continue;   // (the recipe cannot be paid from these tests' bag)
        const int q = std::max(quality, quality_at_least(world, pick, in.min_rarity));
        s.items.push_back(tl_item(slot++, pick, need, q));
        s.req.tags.push_back(pick_of({resource(pick, need, world.item_rarity_facts(pick).rarity_of(q))}, tags));
    }
    return s;
}

/// The facts of a start at `station` in a Riverlands node of `level`, for a player of certification tier `tier`; `taught`
/// says which recipes are known.
static crafting::StartFacts tl_facts(const content::CraftRecipeData& data, const content::CraftStationDef& station, const items::Inventory& bag, const PlayerWorld& world,
                                     int tier, const std::function<bool(int64_t)>& taught, int level = 3, const char* biome = "Riverlands") {
    crafting::StartFacts f;
    f.recipes = &data;
    f.station = &station;
    f.quick = true;
    f.tier_of = [tier](int64_t) { return tier; };
    f.taught = taught;
    f.inventory = &bag;
    f.rarity_facts = [&world](int64_t item) { return world.item_rarity_facts(item); };
    f.tax = crafting::station_tax(station.tax_tag);
    f.copper = 5'000'000;
    f.node.level = level;
    f.node.biome = biome;
    return f;
}

/// The taught test for one recipe set.
static std::function<bool(int64_t)> tl_known(const std::set<int64_t>& known) {
    return [known](int64_t recipe) { return known.count(recipe) != 0; };
}

/// What the quick make pays with, from the plan's takes: the reagent does not count, the general inputs' stacks and rarities do.
static std::vector<crafting::PaidUnits> tl_paid(const crafting::Plan& plan) {
    std::vector<crafting::PaidUnits> paid;
    for (const crafting::Take& t : plan.takes) {
        if (t.lowest_first) continue;
        paid.push_back({t.slot, 1, t.count, items::default_rarity_facts().rarity_of(1)});
    }
    return paid;
}

/// The product facts a quick make reads: its tags (the slot factor and the experience) and its quality bands.
static std::optional<content::ItemSaleFacts> tl_sale(const content::ContentDb& db, int64_t item) {
    std::string error;
    auto f = db.item_sale(item, error);
    if (!f || !f->known) return std::nullopt;
    return f;
}

/// The 39 recipe items the Textile Mill vendor sells, and the recipe each teaches.
static std::map<int64_t, int64_t> tl_poi_teaching(const content::ContentDb& db, const content::VendorData& shops) {
    std::map<int64_t, int64_t> out;   // recipe -> item
    const auto prices = shops.prices.find(tl::kTextileMill);
    if (prices == shops.prices.end()) return out;
    for (const auto& [item, rows] : prices->second) {
        if (rows.empty()) continue;
        std::string error;
        const auto learn = db.item_learn(item, error);
        if (!learn) continue;
        for (const content::ItemLearnRow& row : *learn) out[row.recipe] = item;
    }
    return out;
}

/// The recipes whose teaching item the Textile Mill vendor of Aven's End sells: the 39 recipe items the vendor lists, less the listings its shop's conditions hide for a shopkeeper
/// standing in the outpost's Riverlands zone (the six Emberwake pieces are listed for the Tabletop Mountains).
static std::set<int64_t> tl_riverlands_teaching(const content::ContentDb& db, const content::VendorData& data, const world::CityNodes& nodes) {
    std::set<int64_t> out;
    const vendor::TownShops shops(data);
    vendor::ShopFacts facts;
    facts.nodes = &nodes;
    facts.player_x = facts.npc_x = -875046.9;   // the Textile Mill vendor of Aven's End (the node population's point)
    facts.player_y = facts.npc_y = 606173.9;
    facts.has_npc = true;
    const vendor::TownShops::Holds holds = [&](const content::VendorPredicateRow& p) {
        const auto v = dialogue::evaluate_expression(p.expression, facts);
        return v && v->i != 0;
    };
    for (const auto& [recipe, item] : tl_poi_teaching(db, data)) {
        repl::VendorPurchaseCart cart;
        cart.vendor = tl::kTextileMill;
        cart.items.push_back({item, 1, std::nullopt, std::nullopt});
        if (shops.quote(tl::kTextileMill, cart, shops.default_rate(tl::kTextileMill), holds).ok) out.insert(recipe);
    }
    return out;
}

/// The data of the milestone: 530 recipes, the station sets, the four walls and the 14 recipes that make nothing.
static void test_tl_data(const content::ContentDb& db, const PlayerWorld& world) {
    (void)world;
    std::printf("Tailoring's 530 recipes, its three station sets and the four walls\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    if (!loaded) {
        std::printf("  [FAIL] the recipes do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    const content::CraftRecipeData& data = *loaded;
    size_t recipes = 0, tier0 = 0, tier1 = 0, tier2 = 0, learnable = 0, notused = 0, equipment = 0, black = 0, fuel = 0, filter_inputs = 0, filter_recipes = 0;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != tl::kProfession) continue;
        ++recipes;
        tier0 += r.tier == 0 ? 1 : 0;
        tier1 += r.tier == 1 ? 1 : 0;
        tier2 += r.tier == 2 ? 1 : 0;
        learnable += r.learnable ? 1 : 0;
        notused += r.name.rfind("NOTUSED_", 0) == 0 ? 1 : 0;
        equipment += r.inventory_destination ? 0 : 1;
        black += r.blacklisted ? 1 : 0;
        fuel += r.fuel > 0 ? 1 : 0;
        bool filter = false;
        for (const content::CraftInput& in : r.inputs) {
            if (in.item == 0 && in.filter != 0) {
                ++filter_inputs;
                filter = true;
            }
        }
        filter_recipes += filter ? 1 : 0;
    }
    CHECK_SHOWN(recipes == 530 && tier0 == 119 && tier1 == 170 && tier2 == 241, "Tailoring holds 530 recipes: 119 Novice, 170 Apprentice and 241 Journeyman");
    CHECK_SHOWN(learnable == 471 && notused == 8 && equipment == 0 && black == 0 && fuel == 0, "471 have to be learned (8 are retired NOTUSED_ rows), none wears an Equipment slot, none burns fuel and none is switched off");
    CHECK_SHOWN(filter_inputs == 4 && filter_recipes == 2, "4 inputs on 2 recipes are named by a gather-item filter (contentc 47's table lists their items)");
    size_t build = 0, node_level = 0, village = 0, encampment = 0, biome = 0, tabletop = 0, riverlands = 0, desert_highlands = 0, jungle = 0;
    std::map<std::string, size_t> modifications;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != tl::kProfession) continue;
        for (const content::CraftPred& p : r.preds) {
            if (p.type == "BuildingModification") {
                ++build;
                for (const std::string& m : crafting::split_tags(p.modifications)) modifications[m]++;
            } else if (p.type == "NodeLevel") {
                ++node_level;
                village += p.node_level == 3 ? 1 : 0;
                encampment += p.node_level == 2 ? 1 : 0;
            } else if (p.type == "Biome") {
                ++biome;
                tabletop += p.biomes.find("TabletopMountains") != std::string::npos ? 1 : 0;
                riverlands += p.biomes.find("Riverlands") != std::string::npos ? 1 : 0;
                desert_highlands += p.biomes.find("Desert") != std::string::npos || p.biomes.find("Highlands") != std::string::npos ? 1 : 0;
                jungle += p.biomes.find("Jungle") != std::string::npos ? 1 : 0;
            }
        }
    }
    CHECK_SHOWN(build == 296 && modifications.size() == 3 && modifications["6064634161298145360"] == 101 && modifications["6064634161489313909"] == 105 &&
                    modifications["6064634161492131959"] == 90,
                "296 name the Textile Mill's service building: 101, 105 and 90 recipes of its three modification records");
    CHECK_SHOWN(node_level == 80 && village == 79 && encampment == 1, "80 need a node's level: 79 a Village and 1 an Encampment");
    CHECK_SHOWN(biome == 25 && tabletop == 10 && riverlands == 9 && desert_highlands == 3 && jungle == 3, "25 name a biome: TabletopMountains 10, Riverlands 9, Desert or Highlands 3 and Jungle 3");

    // the 14 that give the server no product: the 8 retired rows and the two artisan shirts of each tier
    std::vector<std::string> no_product;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != tl::kProfession) continue;
        const content::CraftReward* reward = data.reward_of(r);
        if (!reward || reward->containers.empty()) no_product.push_back(r.name);
    }
    CHECK_SHOWN(no_product.size() == 14, "14 rewards give the server no product");
    size_t retired = 0, shirts = 0;
    for (const std::string& name : no_product) {
        retired += name.rfind("NOTUSED_", 0) == 0 ? 1 : 0;
        shirts += name.find("_Artisan_AnimalHusbandry_Shirt") != std::string::npos || name.find("_Artisan_Scribe_Shirt") != std::string::npos ? 1 : 0;
    }
    CHECK_SHOWN(retired == 8 && shirts == 6, "...: the 8 retired NOTUSED_ rows and the Animal Husbandry and Scribe shirts of the three tiers");

    // the station sets
    const content::CraftStationData* stations = nullptr;
    std::string station_error;
    const auto station_data = db.craft_station_data(station_error);
    if (station_data) stations = &*station_data;
    CHECK_SHOWN(stations != nullptr, "the station definitions read");
    if (!stations) return;
    const content::CraftStationDef* t1 = station_named(*stations, "CraftingStation_Tailoring_T1");
    const content::CraftStationDef* t2 = station_named(*stations, "CraftingStation_Tailoring_T2");
    const content::CraftStationDef* t3 = station_named(*stations, "CraftingStation_Tailoring_T3");
    CHECK_SHOWN(t1 && t2 && t3 && t1->max_tier == 0 && t2->max_tier == 1 && t3->max_tier == 2, "the Novice, Apprentice and Journeyman stations reach tiers 0, 1 and 2");
    if (!t1 || !t2 || !t3) return;
    const auto set_size = [&](const content::CraftStationDef& st) {
        size_t n = 0;
        for (const auto& [guid, r] : data.recipes) n += r.profession == tl::kProfession && data.set_holds(st.recipe_set, guid) ? 1 : 0;
        return n;
    };
    CHECK_SHOWN(set_size(*t1) == 119 && set_size(*t2) == 289 && set_size(*t3) == 530, "their recipe sets hold 119, 289 (the 119 and the 170) and 530 (every recipe)");
    size_t t1_outside = 0;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession == tl::kProfession && r.tier == 0 && !data.set_holds(t1->recipe_set, guid)) ++t1_outside;
    }
    CHECK_SHOWN(t1_outside == 0, "every Novice recipe is one of the Novice station's recipes");

    // the retired rows are caught by the service building, not only by the reward
    size_t notused_in_t3 = 0, notused_with_build = 0;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != tl::kProfession || r.name.rfind("NOTUSED_", 0) != 0) continue;
        notused_in_t3 += data.set_holds(t3->recipe_set, guid) ? 1 : 0;
        for (const content::CraftPred& p : r.preds) notused_with_build += p.type == "BuildingModification" ? 1 : 0;
    }
    CHECK_SHOWN(notused_in_t3 == 8 && notused_with_build == 8, "the 8 retired rows stand in the Journeyman set and carry the Textile Mill's predicate (they are refused before their reward)");

    // the five a fresh Novice can start, and the 39 the vendor teaches
    std::string vendor_error;
    auto shops_data = db.vendor_data(vendor_error);
    CHECK_SHOWN(shops_data.has_value(), "the vendor tables read");
    if (!shops_data) return;
    const std::map<int64_t, int64_t> teaching = tl_poi_teaching(db, *shops_data);
    CHECK_SHOWN(teaching.size() == 39, "the Textile Mill vendor lists 39 Tailoring recipe items, each teaching one recipe");
    size_t teach_tier0 = 0, teach_learnable = 0;
    for (const auto& [recipe_guid, item] : teaching) {
        const content::CraftRecipe* r = data.recipe(recipe_guid);
        if (!r) continue;
        teach_tier0 += r->tier == 0 ? 1 : 0;
        teach_learnable += r->learnable ? 1 : 0;
    }
    CHECK_SHOWN(teach_tier0 == 39 && teach_learnable == 39, "all 39 are Novice recipes that have to be learned");
    SAYOK("Tailoring's data");
}

/// The two outpost vendors: their listings, prices, predicates, the node's tax and the node the shop stands in.
static void test_tl_vendors(const content::ContentDb& db) {
    std::printf("the Textile Mill and Hideworks vendors: their listings, prices, predicates and biomes\n");
    std::string error;
    auto data = db.vendor_data(error);
    if (!data) {
        std::printf("  [FAIL] the shops do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    const vendor::TownShops shops(std::move(*data));
    const content::VendorData::Def* textile = shops.def(tl::kTextileMill);
    const content::VendorData::Def* hideworks = shops.def(tl::kHideworks);
    CHECK_SHOWN(textile && textile->name == "ArtisanshipPOI_TextileMill_Vendor" && hideworks && hideworks->name == "ArtisanshipPOI_Hideworks_Vendor",
                "the Textile Mill vendor and the Hideworks vendor of the artisan outposts are in the data");
    if (!textile || !hideworks) return;
    CHECK_SHOWN(shops.is_open(tl::kTextileMill) && shops.is_open(tl::kHideworks), "both shops are switched on");
    CHECK_SHOWN(textile->inventories.size() == 2 && hideworks->inventories.size() == 2, "each has two tabs: Tailoring and Weaving, Leatherworking and Tanning");
    size_t textile_listings = 0, hideworks_listings = 0, textile_8 = 0, textile_43 = 0, hideworks_8 = 0, hideworks_43 = 0, money = 0;
    for (const auto& [shop, items] : shops.data().prices) {
        for (const auto& [item, rows] : items) {
            for (const content::VendorPriceRow& r : rows) {
                if (shop == tl::kTextileMill) {
                    ++textile_listings;
                    textile_8 += r.price == 8 ? 1 : 0;
                    textile_43 += r.price == 43 ? 1 : 0;
                } else if (shop == tl::kHideworks) {
                    ++hideworks_listings;
                    hideworks_8 += r.price == 8 ? 1 : 0;
                    hideworks_43 += r.price == 43 ? 1 : 0;
                } else {
                    continue;
                }
                money += r.currency == items::kMoneyTier ? 1 : 0;
            }
        }
    }
    CHECK_SHOWN(textile_listings == 55 && hideworks_listings == 29, "the Textile Mill vendor lists 55 items (50 Tailoring and 5 Weaving) and the Hideworks vendor 29 (25 Leatherworking and 4 Tanning)");
    CHECK_SHOWN(textile_8 == 16 && textile_43 == 39 && hideworks_8 == 17 && hideworks_43 == 12, "the reagents cost 8 copper and the recipe items 43");
    CHECK_SHOWN(money == textile_listings + hideworks_listings, "every price is in copper");
    // the node's tax is the default table's: half the price, no flat amount
    const vendor::TaxRate rate = shops.default_rate(tl::kTextileMill);
    CHECK_SHOWN(rate.found && std::fabs(rate.percent - 0.5f) < 1e-6f && rate.flat == 0, "the shop's tax tag finds the default node table (50 percent, no flat amount)");
    CHECK_SHOWN(vendor::taxed_unit(8, rate) == 12 && vendor::taxed_unit(43, rate) == 65, "a reagent costs 12 copper with the tax and a recipe item 65");

    // the biome predicates: 12 of each shop, 6 for the Riverlands and 6 for the Tabletop Mountains
    for (const auto& [shop, name] : {std::pair<int64_t, const char*>{tl::kTextileMill, "Textile Mill"}, {tl::kHideworks, "Hideworks"}}) {
        size_t rows = 0, river = 0, table = 0;
        for (const auto& [key, preds] : shops.data().predicates) {
            bool mine = false;
            const content::VendorData::Def* d = shops.def(shop);
            for (const int64_t tab : d->inventories) mine = mine || key.first == tab;
            if (!mine) continue;
            for (const content::VendorPredicateRow& p : preds) {
                ++rows;
                river += p.expression.find("EBiomeType::Riverlands") != std::string::npos ? 1 : 0;
                table += p.expression.find("EBiomeType::TabletopMountains") != std::string::npos ? 1 : 0;
            }
        }
        CHECK_SHOWN(rows == 12 && river == 6 && table == 6, (std::string("the ") + name + " vendor's 12 predicates are 6 Riverlands and 6 TabletopMountains").c_str());
    }

    // GetNodeBiome answers for the node the shop stands in: the Riverlands where the outposts stand, another biome elsewhere
    std::string node_error;
    auto nodes = world::CityNodes::load(db.path(), node_error);
    CHECK_SHOWN(nodes != nullptr, "the city nodes read");
    if (!nodes) return;
    const world::CityNodeInfo* halcyon = nodes->by_name("Halcyon");
    CHECK_SHOWN(halcyon != nullptr, "Halcyon is one of the nodes (a Village of the Riverlands)");
    if (!halcyon) return;
    vendor::ShopFacts river;
    river.nodes = &*nodes;
    river.player_x = river.npc_x = halcyon->x;
    river.player_y = river.npc_y = halcyon->y;
    river.has_npc = true;
    const auto river_biome = dialogue::evaluate_expression("GetNodeBiome() == EBiomeType::Riverlands", river);
    const auto river_other = dialogue::evaluate_expression("GetNodeBiome() != EBiomeType::TabletopMountains", river);
    CHECK_SHOWN(river_biome && river_biome->i == 1 && river_other && river_other->i == 1, "at Halcyon GetNodeBiome is the Riverlands");
    // a Tabletop Mountains zone's node: its point answers that biome, not the Riverlands
    const world::CityNodeInfo* tabletop = nullptr;
    for (const world::CityNodeInfo& n : nodes->nodes()) {
        if (nodes->node_at(n.x, n.y).biome == 17) {   // EBiomeType::TabletopMountains
            tabletop = &n;
            break;
        }
    }
    CHECK_SHOWN(tabletop != nullptr, "a Tabletop Mountains node is in the data");
    if (tabletop) {
        vendor::ShopFacts table;
        table.nodes = &*nodes;
        table.player_x = table.npc_x = tabletop->x;
        table.player_y = table.npc_y = tabletop->y;
        table.has_npc = true;
        const auto is_table = dialogue::evaluate_expression("GetNodeBiome() == EBiomeType::TabletopMountains", table);
        const auto is_river = dialogue::evaluate_expression("GetNodeBiome() == EBiomeType::Riverlands", table);
        CHECK_SHOWN(is_table && is_table->i == 1 && is_river && is_river->i == 0, "at a Tabletop Mountains node GetNodeBiome is TabletopMountains, not the Riverlands");
    }

    // a cart at the Riverlands: every Riverlands listing is sold; a Tabletop one is refused, and a cart of a reagent and a recipe item is priced
    const vendor::TownShops::Holds all = [](const content::VendorPredicateRow&) { return true; };
    const vendor::TownShops::Holds river_only = [&](const content::VendorPredicateRow& p) {
        const auto v = dialogue::evaluate_expression(p.expression, river);
        return v && v->i != 0;
    };
    repl::VendorPurchaseCart cart;
    cart.vendor = tl::kTextileMill;
    cart.items.push_back({tl::kBackTeaching, 1, std::nullopt, std::nullopt});
    cart.items.push_back({tl::kLightBack, 1, std::nullopt, std::nullopt});
    const vendor::TownShops::Quote quote = shops.quote(tl::kTextileMill, cart, rate, river_only);
    CHECK_SHOWN(quote.ok && quote.lines.size() == 2 && quote.total == 65 + 12 && quote.currency == items::kMoneyTier, "a cart of the Back recipe item and a Light Back reagent costs 77 copper at the Riverlands");
    // a Tabletop listing (a recipe item of the Emberwake set) is not sold where the shop stands in the Riverlands
    const auto prices = shops.data().prices.find(tl::kTextileMill);
    int64_t tabletop_item = 0;
    if (prices != shops.data().prices.end()) {
        for (const auto& [item, rows] : prices->second) {
            for (const content::VendorPriceRow& r : rows) {
                const auto key = std::make_pair(r.inventory, r.ord);
                const auto p = shops.data().predicates.find(key);
                if (p == shops.data().predicates.end()) continue;
                for (const content::VendorPredicateRow& pred : p->second) {
                    if (pred.expression.find("TabletopMountains") != std::string::npos) tabletop_item = item;
                }
            }
        }
    }
    CHECK_SHOWN(tabletop_item != 0, "the vendor lists a Tabletop Mountains recipe item");
    if (tabletop_item != 0) {
        repl::VendorPurchaseCart no;
        no.vendor = tl::kTextileMill;
        no.items.push_back({tabletop_item, 1, std::nullopt, std::nullopt});
        const vendor::TownShops::Quote refused = shops.quote(tl::kTextileMill, no, rate, river_only);
        CHECK_SHOWN(!refused.ok && refused.refusal.find("is not for sale to this player here") != std::string::npos,
                    "it is not for sale at the Riverlands (the shop's GetNodeBiome predicate does not hold)");
    }

    // what the window marks unavailable: the six Tabletop Mountains listings at a Riverlands shop, no tab hidden
    {
        const repl::UnavailableList unavailable = shops.unavailable(tl::kTextileMill, river_only);
        size_t marked = 0;
        bool hidden = false;
        for (const repl::UnavailableTab& tab : unavailable) {
            hidden = hidden || tab.inventory.hidden;
            marked += tab.listings.size();
        }
        CHECK_SHOWN(marked == 6 && !hidden, "the window marks the six Tabletop Mountains listings unavailable at the Riverlands, and hides no tab");
    }

    // the same evaluator on the Smithy vendor of the same outpost: its 10 Armorsmithing listings are 5 Riverlands and 5 Tabletop
    {
        const repl::UnavailableList smithy = shops.unavailable(6064630643960643621ll, river_only);
        size_t marked = 0;
        bool hidden = false;
        for (const repl::UnavailableTab& tab : smithy) {
            hidden = hidden || tab.inventory.hidden;
            marked += tab.listings.size();
        }
        CHECK_SHOWN(marked == 5 && !hidden, "the Smithy vendor's five Tabletop Mountains listings are unavailable in the Riverlands too (the shared evaluator)");
    }

    // the two shops open the 39 teaching items, the reagents and the Weaving/Tanning reagents the chain needs
    const auto sold = [&](int64_t shop, int64_t item) {
        const auto p = shops.data().prices.find(shop);
        return p != shops.data().prices.end() && p->second.count(item) != 0;
    };
    CHECK_SHOWN(sold(tl::kTextileMill, tl::kBackTeaching) && sold(tl::kTextileMill, tl::kScribeTeaching), "the Textile Mill vendor sells the recipe items of the Back and the Scribe shirt");
    CHECK_SHOWN(sold(tl::kTextileMill, tl::kLightChest) && sold(tl::kTextileMill, tl::kArtisanBag) && sold(tl::kTextileMill, tl::kArtisanShirt), "and the Light slot reagents, the bag reagent and the shirt reagent");
    CHECK_SHOWN(sold(tl::kTextileMill, tl::kDrawtwineWax) && sold(tl::kTextileMill, tl::kBoltwrapTwine), "... and Weaving's Drawtwine Wax and Boltwrap Twine");
    CHECK_SHOWN(sold(tl::kHideworks, tl::kFleshloosenOil) && sold(tl::kHideworks, tl::kHidecureLye), "the Hideworks vendor sells Tanning's Fleshloosen Oil and Hidecure Lye");
    size_t open_teach = 0;
    const std::map<int64_t, int64_t> teaching = tl_poi_teaching(db, shops.data());
    for (const auto& [recipe, item] : teaching) open_teach += sold(tl::kTextileMill, item) ? 1 : 0;
    CHECK_SHOWN(open_teach == 39, "every one of the 39 teaching items is listed by the open Textile Mill vendor (the shop's biome conditions hide six of them in the Riverlands)");

    // every NPC the data links to them opens them
    size_t instances = 0, assets = 0, opening = 0;
    for (const auto& [instance, shop] : shops.data().instance_links) {
        if (shop != tl::kTextileMill && shop != tl::kHideworks) continue;
        ++instances;
        opening += shops.shop_of(instance, 0) == shop ? 1 : 0;
    }
    for (const auto& [asset, shop] : shops.data().asset_links) {
        if (shop != tl::kTextileMill && shop != tl::kHideworks) continue;
        ++assets;
        opening += shops.shop_of(0, asset) == shop ? 1 : 0;
    }
    CHECK_SHOWN(instances == 9 && assets == 2 && opening == instances + assets, "every one of the 9 NPC instances and the 2 NPC assets linked to the two vendors opens its shop");
    SAYOK("the two outpost vendors");
}

/// What starts at each station and tier, fresh and taught, and the first check each other recipe fails.
static void test_tl_plan(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("what starts at the Tailoring stations, a fresh character, one taught by the shop and one taught every recipe\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    if (!loaded) {
        std::printf("  [FAIL] the recipes do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    const content::CraftRecipeData served = *loaded;
    std::string station_error;
    const auto station_data = db.craft_station_data(station_error);
    CHECK_SHOWN(station_data.has_value(), "the station definitions read");
    if (!station_data) return;
    const content::CraftStationDef* t1 = station_named(*station_data, "CraftingStation_Tailoring_T1");
    const content::CraftStationDef* t2 = station_named(*station_data, "CraftingStation_Tailoring_T2");
    const content::CraftStationDef* t3 = station_named(*station_data, "CraftingStation_Tailoring_T3");
    CHECK_SHOWN(t1 && t2 && t3, "the three Tailoring stations are in the data");
    if (!t1 || !t2 || !t3) return;

    const auto starts = [&](const content::CraftStationDef& station, int tier, const std::function<bool(int64_t)>& taught, int level, const char* biome) {
        int ok = 0;
        std::map<std::string, int> refused;
        std::vector<std::string> refused_names;
        for (const auto& [guid, r] : served.recipes) {
            if (r.profession != tl::kProfession || !served.set_holds(station.recipe_set, guid)) continue;
            const TlStart start = tl_start_for(served, r, 1, world);
            auto bag = bag_of(start.items);
            if (!bag) {
                ++refused["bag"];
                continue;
            }
            const cr::Plan plan = cr::plan_start(start.req, tl_facts(served, station, *bag, world, tier, taught, level, biome));
            if (plan.ok) {
                const content::CraftReward* reward = served.reward_of(r);
                if (!reward || reward->containers.empty()) {
                    ++refused["no reward"];
                    refused_names.push_back(r.name);
                } else {
                    ++ok;
                }
            } else if (plan.refusal.find("has to be learned") != std::string::npos) {
                ++refused["learnable"];
            } else if (plan.refusal.find("needs certification tier") != std::string::npos) {
                ++refused["cert"];
            } else if (plan.refusal.find("made only in the biomes") != std::string::npos) {
                ++refused["biome"];
            } else if (plan.refusal.find("needs a service building") != std::string::npos) {
                bool recipe_building = false;
                for (const content::CraftPred& p : r.preds) recipe_building = recipe_building || p.type == "BuildingModification";
                ++refused[recipe_building ? "building" : "list"];
            } else if (plan.refusal.find("needs a node of level") != std::string::npos) {
                ++refused["node level"];
            } else if (plan.refusal.find("needs tier") != std::string::npos) {
                ++refused["station tier"];
            } else if (plan.refusal.find("filter") != std::string::npos) {
                ++refused["filter"];
            } else {
                ++refused["other"];
            }
        }
        std::printf("    %s at tier %d, node level %d, %s: %d start", station.name.c_str(), tier, level, biome, ok);
        for (const auto& [why, n] : refused) std::printf("; %s %d", why.c_str(), n);
        std::printf("\n");
        for (const std::string& n : refused_names) std::printf("      no reward: %s\n", n.c_str());
        return std::make_pair(ok, refused);
    };

    std::set<int64_t> all_learnable;
    for (const auto& [guid, r] : served.recipes) {
        if (r.profession == tl::kProfession && r.learnable) all_learnable.insert(guid);
    }
    std::set<int64_t> taught_poi, taught_river;
    {
        std::string vendor_error;
        const auto vendors = db.vendor_data(vendor_error);
        std::string node_error;
        const auto nodes = world::CityNodes::load(db.path(), node_error);
        if (vendors) {
            for (const auto& [recipe, item] : tl_poi_teaching(db, *vendors)) taught_poi.insert(recipe);
            if (nodes) taught_river = tl_riverlands_teaching(db, *vendors, *nodes);
        }
    }
    CHECK_SHOWN(taught_poi.size() == 39, "the 39 recipes the open Textile Mill vendor lists a teaching item for");
    CHECK_SHOWN(taught_river.size() == 33, "33 of them are for sale at Aven's End: the six Emberwake items are listed only for a shopkeeper in the Tabletop Mountains");

    const auto fresh1 = starts(*t1, 0, tl_known({}), 0, "Riverlands");
    CHECK_SHOWN(fresh1.first == 5 && fresh1.second.at("learnable") == 84 && fresh1.second.at("node level") == 27 && fresh1.second.at("biome") == 3,
                "a fresh Novice at Aven's End starts 5 of the 119; 84 wait on the learning, 27 on a Village and 3 on another biome");
    const auto poi1 = starts(*t1, 0, tl_known(taught_poi), 0, "Riverlands");
    CHECK_SHOWN(poi1.first == 42 && poi1.second.at("learnable") == 45 && poi1.second.at("node level") == 27 && poi1.second.at("biome") == 3 &&
                    poi1.second.at("no reward") == 2,
                "with all 39 recipe items the vendor lists learned, 42 would start: 45 recipes still wait on the learning, 27 on the node's level, 3 on the biome, and 2 make nothing");
    const auto river1 = starts(*t1, 0, tl_known(taught_river), 0, "Riverlands");
    CHECK_SHOWN(river1.first == 36 && river1.second.at("learnable") == 51 && river1.second.at("node level") == 27 && river1.second.at("biome") == 3 && river1.second.at("no reward") == 2,
                "with the 33 recipe items the shop sells at Aven's End learned, 36 start: 51 recipes still wait on the learning, 27 on the node's level, 3 on the biome, and 2 make nothing");
    const auto taught1 = starts(*t1, 0, tl_known(all_learnable), 0, "Riverlands");
    CHECK_SHOWN(taught1.first == 85 && taught1.second.at("node level") == 29 && taught1.second.at("biome") == 3 && taught1.second.at("no reward") == 2,
                "taught every recipe, the Novice starts 85: 29 need a Village, 3 another biome and 2 make nothing");
    const auto fresh2a = starts(*t2, 0, tl_known({}), 3, "Riverlands");
    CHECK_SHOWN(fresh2a.first == 32 && fresh2a.second.at("learnable") == 245 && fresh2a.second.at("cert") == 9 && fresh2a.second.at("biome") == 3,
                "a fresh Novice at Halcyon's Apprentice station starts 32 of the 289: 245 wait on the learning, 9 on the certification, 3 on the biome");
    const auto fresh2b = starts(*t2, 1, tl_known({}), 3, "Riverlands");
    CHECK_SHOWN(fresh2b.first == 38 && fresh2b.second.at("learnable") == 245 && fresh2b.second.at("biome") == 6, "a fresh Apprentice starts 38: 245 on the learning and 6 on the biome");
    const auto taught2 = starts(*t2, 1, tl_known(all_learnable), 3, "Riverlands");
    CHECK_SHOWN(taught2.first == 186 && taught2.second.at("building") == 92 && taught2.second.at("biome") == 7 && taught2.second.at("no reward") == 4,
                "taught every recipe, the Apprentice starts 186: 92 wait on the Textile Mill's service building, 7 on the biome and 4 make nothing");
    const auto taught3 = starts(*t3, 2, tl_known(all_learnable), 3, "Riverlands");
    CHECK_SHOWN(taught3.first == 216 && taught3.second.at("building") == 293 && taught3.second.at("biome") == 10 && taught3.second.at("no reward") == 6,
                "taught every recipe, a Journeyman at a placed Journeyman station starts 216 of the 530: 293 need the building, 10 another biome and 6 make nothing");
    CHECK_SHOWN(!taught2.second.count("other") && !taught3.second.count("other") && !taught3.second.count("filter"), "no recipe is refused for a check this milestone does not name");
    CHECK_SHOWN(fresh1.first + fresh1.second.at("learnable") + fresh1.second.at("node level") + fresh1.second.at("biome") == 119,
                "the fresh Novice station's five groups add up to its 119 recipes");

    // the exact refusal texts of the checks, one real row each
    const auto plan_of = [&](const char* name, const content::CraftStationDef& station, int tier, const std::set<int64_t>& known, int level, const char* biome) {
        const content::CraftRecipe* r = recipe_named(served, name);
        if (!r) return cr::Plan{};
        const TlStart start = tl_start_for(served, *r, 1, world);
        auto bag = bag_of(start.items);
        return bag ? cr::plan_start(start.req, tl_facts(served, station, *bag, world, tier, tl_known(known), level, biome)) : cr::Plan{};
    };
    {
        const cr::Plan back = plan_of("Tailoring_T1_Armor_AcademicAegis_Back", *t1, 0, {}, 0, "Riverlands");
        CHECK_SHOWN(!back.ok && back.refusal == "Tailoring_T1_Armor_AcademicAegis_Back has to be learned, and the character was not taught it",
                    "an unlearned recipe: \"<recipe> has to be learned, and the character was not taught it\"");
    }
    {
        const std::set<int64_t> taught = {tl::kSiegeAithanahr};
        const cr::Plan banner = plan_of("Tailoring_T1_SiegeDeclarationBanner_Aithanahr", *t1, 0, taught, 0, "Riverlands");
        CHECK_SHOWN(!banner.ok && banner.refusal == "Tailoring_T1_SiegeDeclarationBanner_Aithanahr: it needs a node of level 3 and the node is level 0",
                    "a Village recipe at a Wilderness node: \"<recipe>: it needs a node of level 3 and the node is level 0\"");
    }
    {
        const cr::Plan ember = plan_of("Tailoring_T1_Armor_Emberwake_Chest", *t1, 0, {}, 0, "Riverlands");
        CHECK_SHOWN(!ember.ok && ember.refusal == "Tailoring_T1_Armor_Emberwake_Chest: it is made only in the biomes TabletopMountains and the node is in Riverlands",
                    "a Tabletop Mountains recipe in the Riverlands: \"<recipe>: it is made only in the biomes TabletopMountains and the node is in Riverlands\"");
    }
    {
        const std::set<int64_t> taught = {tl::kAnnalistRecipe};
        const cr::Plan annalist = plan_of("Tailoring_T2_Armor_Annalist_Chest", *t2, 1, taught, 3, "Riverlands");
        CHECK_SHOWN(!annalist.ok && annalist.refusal == "Tailoring_T2_Armor_Annalist_Chest: it needs a service building in the node, and no node has one yet",
                    "a service-building recipe: \"<recipe>: it needs a service building in the node, and no node has one yet\"");
    }
    {
        const cr::Plan befallen = plan_of("Tailoring_T3_Artisan_Mining_Shirt_Befallen", *t3, 2, {tl::kMiningShirtBefallen}, 3, "Riverlands");
        CHECK_SHOWN(!befallen.ok && befallen.refusal == "Tailoring_T3_Artisan_Mining_Shirt_Befallen: it needs a service building in the node, and no node has one yet",
                    "the Befallen Forge list's own predicate is the service-building text too");
    }
    {
        // the station tier: a real Journeyman recipe in a station that reaches Apprentice (the data has no such station; its shape is tested on a copy)
        content::CraftStationDef low = *t3;
        low.max_tier = 1;
        low.display_name = "Tailoring - Apprentice";
        const cr::Plan toren = plan_of("Tailoring_T3_Armor_TorenAdministrators_Chest", low, 2, {tl::kTorenChestRecipe}, 3, "Riverlands");
        CHECK_SHOWN(!toren.ok && toren.refusal == "Tailoring_T3_Armor_TorenAdministrators_Chest needs tier 2 and Tailoring - Apprentice reaches 1",
                    "a station under the recipe's tier: \"<recipe> needs tier 2 and <station> reaches 1\"");
    }
    {
        const cr::Plan arcane = plan_of("Tailoring_T2_Armor_ArcaneAegis_Back", *t1, 1, {tl::kArcaneBackRecipe}, 0, "Riverlands");
        CHECK_SHOWN(!arcane.ok && arcane.refusal == "Tailoring_T2_Armor_ArcaneAegis_Back is not one of the recipes of Tailoring - Novice",
                    "an Apprentice recipe at the Novice station: \"<recipe> is not one of the recipes of <station>\"");
    }

    // the Novice piece's price, time and takes; its quality, durability and experience
    const content::CraftRecipe* chest = served.recipe(tl::kChestRecipe);
    CHECK_SHOWN(chest != nullptr, "the AcademicAegis Chest's recipe is in the data");
    if (!chest) return;
    const TlStart chest_start = tl_start_for(served, *chest, 1, world);
    auto chest_bag = bag_of(chest_start.items);
    CHECK_SHOWN(chest_bag.has_value(), "a bag of 4 Daffodil thread, a Grem skin and a Light Chest reagent");
    if (!chest_bag) return;
    const cr::Plan chest_plan = cr::plan_start(chest_start.req, tl_facts(served, *t1, *chest_bag, world, 0, tl_known({}), 0, "Riverlands"));
    CHECK_SHOWN(chest_plan.ok, "a fresh Novice starts the AcademicAegis Chest at Novice Tailoring");
    if (!chest_plan.ok) {
        std::printf("    %s\n", chest_plan.refusal.c_str());
        return;
    }
    CHECK_SHOWN(chest_plan.unit_cost == 50 && chest_plan.unit_taxed == 75 && chest_plan.cost == 75, "it costs 50 copper, 75 with the Textile Mill service tax of half the price");
    CHECK_SHOWN(chest_plan.duration_ms == 2000, "it runs 2 s (the recipe's base duration)");
    CHECK_SHOWN(chest_plan.takes.size() == 3 && chest_plan.takes.front().lowest_first && chest_plan.takes.front().record == tl::kLightChest, "the Light reagent is taken first");
    const auto made = cr::roll_craft(served, *chest_plan.recipe, tl_paid(chest_plan), chest_plan.ratings, 1, 7, [&](int64_t item) { return tl_sale(db, item); });
    CHECK_SHOWN(made.has_value() && made->products.size() == 1 && made->products.front().item == tl::kChestItem && made->products.front().quantity == 1,
                "the quick make makes one Gear_Armor_Light_AcademicAegis_Chest");
    if (made) {
        CHECK_SHOWN(made->quality == 1 && made->rarity == 0, "all-Common materials make it quality 1 (Common)");
        CHECK_SHOWN(made->profession_xp == 200, "it pays 200 Tailoring experience (Novice 100 x the Common rarity x the chest's 2.0 slot factor)");
        CHECK_SHOWN(made->adventuring_xp > 0, "and Adventuring experience by the crafting formula");
    }
    std::string item_error;
    const auto chest_item = db.item(tl::kChestItem, item_error);
    CHECK_SHOWN(chest_item && std::fabs(chest_item->max_durability_at(1) - 13.0) < 0.01 && chest_item->equip_slots == std::vector<uint8_t>{items::equip::Chest},
                "the chest's durability at quality 1 is 13 and it is worn in the Chest slot");
    // the Back: the learnable Novice piece the shop's teaching item opens, worn on the back
    const content::CraftRecipe* back = served.recipe(tl::kBackRecipe);
    CHECK_SHOWN(back != nullptr && back->learnable && back->tier == 0, "the AcademicAegis Back is a learnable Novice recipe (the Textile Mill sells its item)");
    if (back) {
        const TlStart start = tl_start_for(served, *back, 1, world);
        auto bag = bag_of(start.items);
        const cr::Plan plan = bag ? cr::plan_start(start.req, tl_facts(served, *t1, *bag, world, 0, tl_known({tl::kBackRecipe}), 0, "Riverlands")) : cr::Plan{};
        CHECK_SHOWN(plan.ok && plan.cost == 75, "learned, the Back starts at the Novice station for 75 copper");
        const auto back_made = plan.ok ? cr::roll_craft(served, *plan.recipe, tl_paid(plan), plan.ratings, 1, 11, [&](int64_t item) { return tl_sale(db, item); }) : std::nullopt;
        CHECK_SHOWN(back_made.has_value() && !back_made->products.empty() && back_made->products.front().item == tl::kBackItem && back_made->quality == 1 && back_made->profession_xp == 120,
                    "it makes the Back at quality 1 and pays 120 Tailoring experience (100 x the back's 1.2 slot factor)");
        std::string back_error;
        const auto back_item = db.item(tl::kBackItem, back_error);
        CHECK_SHOWN(back_item && std::fabs(back_item->max_durability_at(1) - 11.0) < 0.01 && back_item->equip_slots == std::vector<uint8_t>{items::equip::Back},
                    "the Back's durability at quality 1 is 11 and it is worn in the Back slot");
    }
    // all-Uncommon materials and a quality rating: the quality and the durability follow both
    std::vector<cr::PaidUnits> uncommon;
    for (const cr::Take& t : chest_plan.takes) {
        if (!t.lowest_first) uncommon.push_back({t.slot, 1, t.count, 1});
    }
    const auto better = cr::roll_craft(served, *chest_plan.recipe, uncommon, chest_plan.ratings, 1, 7, [&](int64_t item) { return tl_sale(db, item); });
    CHECK_SHOWN(better.has_value() && better->quality == 6 && better->rarity == 1 && better->profession_xp == 400,
                "all-Uncommon materials make quality 6, Uncommon, and pay 400 (the band's first quality and twice the experience)");
    cr::Ratings rating;
    rating.quality = 1503;
    const auto rated = cr::roll_craft(served, *chest_plan.recipe, tl_paid(chest_plan), rating, 1, 7, [&](int64_t item) { return tl_sale(db, item); });
    CHECK_SHOWN(rated.has_value() && rated->quality_added > 7.9 && rated->quality == 9,
                "a quality rating of 1,503 adds about 8 to the all-Common base of 1: the piece comes out at quality 9 (Uncommon)");
    std::string error_item;
    const auto rare_durability = db.item(tl::kChestItem, error_item);
    CHECK_SHOWN(rare_durability && std::fabs(rare_durability->max_durability_at(9) - 14.0) < 0.01, "at quality 9 its durability is 14 (the item's MaxDurability in the Uncommon band)");

    // the 6 shirts make nothing: the reward table holds no item, which the server refuses at the start
    const content::CraftRecipe* scribe = served.recipe(tl::kScribeRecipe);
    CHECK_SHOWN(scribe != nullptr && scribe->learnable && scribe->tier == 0, "the Scribe shirt is a Novice recipe that has to be learned (the Textile Mill sells its item)");
    if (scribe) {
        const content::CraftReward* reward = served.reward_of(*scribe);
        CHECK_SHOWN(reward && reward->containers.empty(), "its reward table holds no item: the server says it makes nothing");
        const TlStart start = tl_start_for(served, *scribe, 1, world);
        auto bag = bag_of(start.items);
        const cr::Plan plan = bag ? cr::plan_start(start.req, tl_facts(served, *t1, *bag, world, 0, tl_known({tl::kScribeRecipe}), 0, "Riverlands")) : cr::Plan{};
        CHECK_SHOWN(plan.ok, "plan_start lets it through (the reward is read when the craft starts)");
    }
    SAYOK("the station counts and the crafted numbers");
}

/// The live chain of the five recipes a fresh Novice can feed: the thread, the skin, the Light reagents, the bag reagent and the sail's filters.
static void test_tl_feed(const content::ContentDb& db, const PlayerWorld& world) {
    (void)world;
    std::printf("the five recipes a fresh Novice can feed, and where the live world gives their inputs\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    if (!loaded) {
        std::printf("  [FAIL] the recipes do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    const content::CraftRecipeData& data = *loaded;
    const content::CraftRecipe* chest = recipe_named(data, "Tailoring_T1_Armor_AcademicAegis_Chest");
    const content::CraftRecipe* legs = recipe_named(data, "Tailoring_T1_Armor_AcademicAegis_Legs");
    const content::CraftRecipe* feet = recipe_named(data, "Tailoring_T1_Armor_AcademicAegis_Feet");
    const content::CraftRecipe* rucksack = recipe_named(data, "Tailoring_T1_Bag_SimpleRucksack");
    const content::CraftRecipe* sail = recipe_named(data, "Tailoring_T1_Ship_Naval_Medium_Sail_2");
    CHECK_SHOWN(chest && legs && feet && rucksack && sail, "the five recipes are in the data");
    if (!chest || !legs || !feet || !rucksack || !sail) return;
    const auto input_of = [](const content::CraftRecipe& r, int64_t item) {
        for (const content::CraftInput& in : r.inputs) {
            if (in.item == item) return in.quantity;
        }
        return 0;
    };
    CHECK_SHOWN(input_of(*chest, tl::kThreadDaffodil) == 4 && input_of(*chest, tl::kSkinGrem) == 1 && input_of(*chest, tl::kLightChest) == 1,
                "the chest takes 4 Daffodil thread, 1 Grem skin and the Light Chest reagent");
    CHECK_SHOWN(input_of(*legs, tl::kThreadDaffodil) == 4 && input_of(*legs, tl::kLightLegs) == 1 && input_of(*legs, tl::kSkinGrem) == 1 &&
                    input_of(*feet, tl::kThreadDaffodil) == 3 && input_of(*feet, tl::kLightFeet) == 1 && input_of(*feet, tl::kSkinGrem) == 1,
                "the legs and the feet take the same with their own Light reagents (4 and 3 thread)");
    CHECK_SHOWN(input_of(*rucksack, tl::kThreadCoarse) == 5 && input_of(*rucksack, tl::kArtisanBag) == 1, "the rucksack takes 5 Coarse thread and the Artisan Bag reagent");
    bool sail_plant = false, sail_wood = false;
    for (const content::CraftInput& in : sail->inputs) {
        if (in.item != 0) continue;
        if (in.filter == tl::kPlantFilter) sail_plant = in.quantity == 75 && data.filter_admits(in.filter, tl::kPlantDaffodil);
        if (in.filter == tl::kSailWoodFilter) sail_wood = in.quantity == 25 && data.filter_admits(in.filter, tl::kOak);
    }
    CHECK_SHOWN(sail_plant && sail_wood, "the sail takes 75 plants and 25 woods by their filters; Daffodil is one of the plants and Oak one of the woods");

    // the thread and the skin are made at the Novice processing stations of the same outpost
    std::string station_error;
    const auto stations = db.craft_station_data(station_error);
    const content::CraftStationDef* weaving = stations ? station_named(*stations, "ProcessingStation_Weaving_T1") : nullptr;
    const content::CraftStationDef* tanning = stations ? station_named(*stations, "ProcessingStation_Tanning_T1") : nullptr;
    CHECK_SHOWN(weaving && tanning, "the Novice Weaving and Tanning stations are in the data");
    if (!weaving || !tanning) return;
    const content::CraftRecipe* daffodil = recipe_named(data, "Weaving_T1_Thread_Daffodil");
    const content::CraftRecipe* snowdrop = recipe_named(data, "Weaving_T1_Thread_Snowdrop");
    const content::CraftRecipe* coarse = recipe_named(data, "Weaving_T1_Thread_Coarse");
    const content::CraftRecipe* grem = recipe_named(data, "Tanning_T1_SkinCarcass_Grem");
    CHECK_SHOWN(daffodil && snowdrop && coarse && grem && data.set_holds(weaving->recipe_set, daffodil->guid) && data.set_holds(weaving->recipe_set, snowdrop->guid) &&
                    data.set_holds(weaving->recipe_set, coarse->guid) && data.set_holds(tanning->recipe_set, grem->guid),
                "they stand in the Weaving and Tanning Novice stations' sets");
    if (!daffodil || !snowdrop || !coarse || !grem) return;
    CHECK_SHOWN(input_of(*daffodil, tl::kPlantDaffodil) == 1 && input_of(*daffodil, tl::kDrawtwineWax) == 1, "Daffodil thread needs one Daffodil and Drawtwine Wax");
    CHECK_SHOWN(input_of(*snowdrop, tl::kPlantSnowdrop) == 1 && input_of(*snowdrop, tl::kDrawtwineWax) == 1, "Snowdrop thread needs one Snowdrop and Drawtwine Wax");
    bool coarse_plant_filter = false;
    for (const content::CraftInput& in : coarse->inputs) {
        if (in.item == 0 && in.filter == tl::kPlantFilter) coarse_plant_filter = data.filter_admits(in.filter, tl::kPlantDaffodil);
    }
    CHECK_SHOWN(coarse_plant_filter && input_of(*coarse, tl::kDrawtwineWax) == 1, "Coarse thread takes any of the 22 gathered plants and Drawtwine Wax");
    CHECK_SHOWN(input_of(*grem, tl::kCarcassGrem) == 1 && input_of(*grem, tl::kFleshloosenOil) == 1, "the Grem skin needs a Grem carcass and Fleshloosen Oil");

    // the reagents the chain takes are on the open shops (the Textile Mill's and the Hideworks')
    std::string vendor_error;
    const auto vendors = db.vendor_data(vendor_error);
    CHECK_SHOWN(vendors.has_value(), "the vendor tables read");
    if (!vendors) return;
    const vendor::TownShops shops(*vendors);
    const auto sold = [&](int64_t shop, int64_t item) {
        const auto p = shops.data().prices.find(shop);
        return p != shops.data().prices.end() && p->second.count(item) != 0;
    };
    CHECK_SHOWN(sold(tl::kTextileMill, tl::kDrawtwineWax) && sold(tl::kTextileMill, tl::kLightChest) && sold(tl::kTextileMill, tl::kLightLegs) &&
                    sold(tl::kTextileMill, tl::kLightFeet) && sold(tl::kTextileMill, tl::kArtisanBag),
                "the two threads' wax, the three Light reagents and the bag reagent are on the open Textile Mill vendor");
    CHECK_SHOWN(sold(tl::kHideworks, tl::kFleshloosenOil), "the Grem skin's oil is on the open Hideworks vendor");
    // the plants and the oak are gatherable: their defs name Herbalism and Lumberjacking
    std::string gather_error;
    const auto gather = db.gather_data(gather_error);
    CHECK_SHOWN(gather.has_value(), "the gathering definitions read");
    if (gather) {
        bool daffodil_def = false, oak_def = false;
        int64_t herbalism = 0, lumber = 0;
        for (const auto& [guid, d] : gather->defs) {
            if (d.name.rfind("Plant_Daffodil_", 0) == 0 && d.cert_tier == 0) {
                daffodil_def = true;
                herbalism = d.profession_guid;
            }
            if (d.name.rfind("Tree_Oak_", 0) == 0 && d.cert_tier == 0) {
                oak_def = true;
                lumber = d.profession_guid;
            }
        }
        CHECK_SHOWN(daffodil_def && oak_def && herbalism != 0 && lumber != 0 && herbalism != lumber, "Daffodil and Oak have Novice gathering definitions of two professions (Herbalism and Lumberjacking)");
    }

    // the banners' reagent is not for sale in the open world
    size_t siege_inputs = 0, open_siege = 0;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != tl::kProfession || r.name.find("SiegeDeclarationBanner_") == std::string::npos) continue;
        for (const content::CraftInput& in : r.inputs) {
            if (in.item == 0) continue;
            std::string item_name;
            if (const auto rec = db.item(in.item, item_name)) {
                if (rec->name.find("SiegeDeclaration_") == std::string::npos) continue;
                ++siege_inputs;
                for (const int64_t shop : shops.open_shops()) {
                    const auto p = shops.data().prices.find(shop);
                    open_siege += p != shops.data().prices.end() && p->second.count(in.item) != 0 ? 1 : 0;
                }
            }
        }
    }
    CHECK_SHOWN(siege_inputs == 26 && open_siege == 0, "the 26 declaration banners' reagent is on no open shop (the Barracks is closed)");
    SAYOK("the live chain");
}

/// What is held back: the T3 station (the construction system's), the recipe items only the PTR defs list, and the siege banners.
static void test_tl_holdbacks(const content::ContentDb& db) {
    std::printf("what the milestone holds back, with the counts\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    if (!loaded) {
        std::printf("  [FAIL] the recipes do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    const content::CraftRecipeData& data = *loaded;
    std::string station_error;
    const auto stations = db.craft_station_data(station_error);
    CHECK_SHOWN(stations.has_value(), "the stations read");
    if (!stations) return;
    const content::CraftStationDef* t2 = station_named(*stations, "CraftingStation_Tailoring_T2");
    const content::CraftStationDef* t3 = station_named(*stations, "CraftingStation_Tailoring_T3");
    const content::CraftStationDef* t4 = station_named(*stations, "CraftingStation_Tailoring_T4");
    const content::CraftStationDef* t5 = station_named(*stations, "CraftingStation_Tailoring_T5");
    CHECK_SHOWN(t2 && t3 && t4 && t5 && t2->max_tier == 1 && t3->max_tier == 2 && t4->max_tier == 3 && t5->max_tier == 4, "the four station definitions still name their tiers");
    size_t only_t3 = 0;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != tl::kProfession) continue;
        only_t3 += !data.set_holds(t2->recipe_set, guid) ? 1 : 0;
    }
    CHECK_SHOWN(only_t3 == 241, "241 recipes stand only in the Journeyman set (and its Befallen Forge list), so they wait on a station no live node has");
    // the five list-predicate rows: the Befallen Forge's own condition, on the tier-2 recipes without a recipe predicate
    std::set<int64_t> list_only;
    for (const auto& [list, recs] : data.list_recipes) {
        if (!data.list_preds.count(list)) continue;
        for (const int64_t recipe : recs) {
            const content::CraftRecipe* r = data.recipe(recipe);
            if (!r || r->profession != tl::kProfession || r->tier != 2 || !r->preds.empty()) continue;
            if (data.set_holds(t3->recipe_set, recipe) && !data.set_holds(t2->recipe_set, recipe)) list_only.insert(recipe);
        }
    }
    CHECK_SHOWN(list_only.size() == 5, "5 Journeyman recipes are refused by the Befallen Forge list's own predicate (their recipe has none)");

    // the 39 teaching items of the shop, the 7 of the Textile Mill building's vendor and the 403 that only the PTR defs list
    std::string vendor_error;
    const auto vendors = db.vendor_data(vendor_error);
    CHECK_SHOWN(vendors.has_value(), "the vendor tables read");
    if (!vendors) return;
    const std::map<int64_t, int64_t> poi = tl_poi_teaching(db, *vendors);
    size_t learnable = 0, at_poi = 0, at_building = 0, from_gm = 0, no_source = 0;
    std::set<int64_t> from_building, from_ptr;
    // the Textile Mill building's vendor: its priced recipe items (the building is not built, the items exist in the data)
    {
        const auto prices = vendors->prices.find(6064632269665140740ll);   // ServiceBuilding_TextileMill_Tailoring_Vendor
        if (prices != vendors->prices.end()) {
            for (const auto& [item, rows] : prices->second) {
                if (rows.empty()) continue;
                std::string e;
                const auto learn = db.item_learn(item, e);
                if (!learn) continue;
                for (const content::ItemLearnRow& row : *learn) from_building.insert(row.recipe);
            }
        }
        CHECK_SHOWN(!from_building.empty(), "the Textile Mill building's vendor lists recipe items (147 listings)");
    }
    // the 26 GM vendors worldd spawns for the PTR defs: their stock is the items those defs list (no price rows: they are free)
    {
        std::string gm_error;
        const auto gm = vendor::GmVendors::load(db, gm_error);
        CHECK_SHOWN(gm.has_value(), "the PTR vendor defs load as the GM vendors the world spawns");
        if (gm) {
            for (const vendor::GmVendor& v : gm->vendors()) {
                if (v.name.rfind("PTR_ONLY_Vendor_Recipes_", 0) != 0) continue;
                for (const int64_t item : v.items) {
                    std::string e;
                    const auto learn = db.item_learn(item, e);
                    if (!learn) continue;
                    for (const content::ItemLearnRow& row : *learn) from_ptr.insert(row.recipe);
                }
            }
        }
    }
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != tl::kProfession || !r.learnable) continue;
        ++learnable;
        if (poi.count(guid)) {
            ++at_poi;
            continue;
        }
        if (from_ptr.count(guid)) {
            ++from_gm;
            continue;
        }
        if (from_building.count(guid)) {
            ++at_building;
            continue;
        }
        ++no_source;
    }
    CHECK_SHOWN(learnable == 471 && at_poi == 39 && from_gm == 403 && at_building == 7 && no_source == 22,
                "of the 471 recipes that have to be learned, 39 are taught by the open shop's items, 403 by items only the PTR defs list (worldd's GM vendors carry them), 7 by the building's vendor and 22 by none");
    size_t ptr_defs = 0;
    for (const auto& [guid, def] : vendors->defs) ptr_defs += def.name.rfind("PTR_ONLY_Vendor_", 0) == 0 ? 1 : 0;
    CHECK_SHOWN(ptr_defs == 26, "the 26 PTR_ONLY vendor defs are the 26 GM vendors the world stands in front of the login spot (no NPC of the game's own links carries their items)");

    // the 26 siege declarations: the Barracks shop, two currencies and a product with no use
    int64_t barracks_guid = 0;
    for (const auto& [guid, def] : vendors->defs) {
        if (def.name == "Barracks_Vendor") barracks_guid = guid;
    }
    CHECK_SHOWN(barracks_guid != 0, "the Barracks vendor is in the data");
    if (barracks_guid == 0) return;
    const vendor::TownShops shops(*vendors);
    CHECK_SHOWN(!shops.is_open(barracks_guid), "the Barracks shop is not one of the open shops (the sieges phase owns it)");
    size_t siege_recipes = 0, declarations = 0, barracks_items = 0, two_currency = 0;
    bool embroidered_fed = false;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != tl::kProfession) continue;
        if (r.name == "Tailoring_T1_EmbroideredSiegeBanner") {
            ++siege_recipes;
            embroidered_fed = !r.inputs.empty();
            for (const content::CraftInput& in : r.inputs) embroidered_fed = embroidered_fed && in.item != 0;
            continue;
        }
        if (r.name.find("SiegeDeclarationBanner_") == std::string::npos) continue;
        ++siege_recipes;
        ++declarations;
        for (const content::CraftInput& in : r.inputs) {
            if (in.item == 0) continue;
            std::string e;
            const auto item = db.item(in.item, e);
            if (!item || item->name.find("SiegeDeclaration_") == std::string::npos) continue;
            const auto prices = vendors->prices.find(barracks_guid);
            if (prices == vendors->prices.end()) continue;
            const auto it = prices->second.find(in.item);
            if (it == prices->second.end()) continue;
            ++barracks_items;
            bool money = false, siege_currency = false;
            for (const content::VendorPriceRow& row : it->second) {
                money = money || row.currency == items::kMoneyTier;
                siege_currency = siege_currency || row.currency != items::kMoneyTier;
            }
            two_currency += money && siege_currency ? 1 : 0;
        }
    }
    CHECK_SHOWN(siege_recipes == 27 && declarations == 26 && embroidered_fed, "Tailoring makes 27 siege banners: the Embroidered one (its input is a plain item) and 26 declarations");
    CHECK_SHOWN(barracks_items == 26 && two_currency == 26, "the 26 declaration reagents are 26 Barracks listings, each with a copper price and a second currency");
    const content::CraftRecipe* aithanahr = recipe_named(data, "Tailoring_T1_SiegeDeclarationBanner_Aithanahr");
    if (aithanahr) {
        int64_t reagent = 0;
        for (const content::CraftInput& in : aithanahr->inputs) {
            std::string e;
            const auto item = db.item(in.item, e);
            if (item && item->name.find("SiegeDeclaration_Aithanahr") != std::string::npos) reagent = in.item;
        }
        const content::CraftReward* reward = data.reward_of(*aithanahr);
        const std::vector<crafting::Product> products = reward ? crafting::roll_products(*reward, 1, 0) : std::vector<crafting::Product>{};
        const int64_t product = products.empty() ? 0 : products.front().item;
        std::string e;
        const auto item = db.item(product, e);
        const auto use = product != 0 ? db.item_use(product, e) : std::nullopt;
        CHECK_SHOWN(reagent != 0 && item && item->name == "Consumable_SiegeDeclarationBanner_Aithanahr" && !use,
                    "the declaration's reagent is sold by the Barracks and its product has no use at all: it declares a siege the server does not have");
    }
    SAYOK("what is held back");
}
