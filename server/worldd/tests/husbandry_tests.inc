// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  husbandry_tests.inc: Animal Husbandry at its stations (the Animal Husbandry milestone)
//
//  The real content.db: 91 recipes, 58 retired and 33 live (6 Novice, 16 Apprentice, 11 Journeyman).  The live Station is Animal
//  Husbandry - Apprentice (ProcessingStation_AnimalHusbandry_T2, tier 1 at most, a queue of 2): its set holds 21 of the 33, the 6
//  Novice and 15 Apprentice.  The Apprentice list carries the data's single "cCount" availability row, the enum's own count (not a
//  reason), which names no condition; before the milestone every recipe of that list was refused as an unknown condition.  The
//  recipes' inputs are the trainable animals: 9 of the 21 start from a hunt in the live Riverlands' 17 hunting kinds (Bear, Horse,
//  Pallas Cat and Wolf at Novice, with the starting bow; Deer, Raptor, Skinwalker, Tidesnapper and Tidebreaker at Apprentice, which
//  need an Apprentice Hunting Bow that no open shop sells and no craft gives yet), and the other 12 wait on
//  an item the live world has no source for (the two crate mules' vendors, the hunting kinds outside the Riverlands, the two
//  livestock recipes whose adult comes from Farming's recipe that takes the baby they make, and the two that have no source at
//  all).  A finished job pays 10 times the processing experience (500 a Novice unit, 600 an Apprentice one) and makes one product
//  from the recipe's reward table: a mount, a beast of burden or a livestock item.  The 31 mount products are all in the mount
//  table with SummonMount; the mount code makes their actor and names their rider animation.  The 11 Journeyman recipes are
//  held back with the Journeyman station that only the Agricultural Supply building spawns.
//  Included by test_worldd_professions.cpp after its helpers and cooking_tests.inc, before main().
// ============================================================================
#include "crafting/station.h"
#include "pop_golden_region.h"
#include "stats/character_stats.h"
#include "vendor/town_shops.h"

namespace ah {
constexpr int64_t kHusbandry = 108752756954758ll;
constexpr int64_t kCooking = 108752757020296ll;

/// The 9 recipes of the 21 at Animal Husbandry - Apprentice whose input a Riverlands hunt gives.
const char* const kFromHunt[9] = {"AnimalHusbandry_T1_Train_Bear", "AnimalHusbandry_T1_Train_Horse", "AnimalHusbandry_T1_Train_PallasCat", "AnimalHusbandry_T1_Train_Wolf",
                                  "AnimalHusbandry_T2_Train_Deer", "AnimalHusbandry_T2_Train_Raptor", "AnimalHusbandry_T2_Train_Skinwalker", "AnimalHusbandry_T2_Train_TideSnapper",
                                  "AnimalHusbandry_T2_Train_Tidebreaker"};
/// Their hunt and the chance of the trainable per hunt: the other line is the carcass (8:2), or the trainable is the only subtable.
struct HuntDrop {
    const char* recipe;
    const char* hunt;
    int         percent;
};
const HuntDrop kDrop[9] = {{"AnimalHusbandry_T1_Train_Bear", "Hunting_Bear", 20},           {"AnimalHusbandry_T1_Train_Horse", "Hunting_Horse", 100},
                           {"AnimalHusbandry_T1_Train_PallasCat", "Hunting_PallasCat", 100}, {"AnimalHusbandry_T1_Train_Wolf", "Hunting_Wolf", 20},
                           {"AnimalHusbandry_T2_Train_Deer", "Hunting_Deer", 100},           {"AnimalHusbandry_T2_Train_Raptor", "Hunting_Swiftclaw", 20},
                           {"AnimalHusbandry_T2_Train_Skinwalker", "Hunting_Skinwalker", 20}, {"AnimalHusbandry_T2_Train_TideSnapper", "Hunting_Tidesnapper", 100},
                           {"AnimalHusbandry_T2_Train_Tidebreaker", "Hunting_Otter", 20}};
/// The 12 recipes of the 21 whose input the live world has no source for, and the source it would need.
const char* const kNoSource[12] = {"AnimalHusbandry_T1_Train_ScorpionBroodling",  "AnimalHusbandry_T1_Train_Daystrider_CrateMule", "AnimalHusbandry_T2_Livestock_Baby_Chicken",
                                   "AnimalHusbandry_T2_Livestock_Baby_Cow",       "AnimalHusbandry_T2_Train_Bork",                 "AnimalHusbandry_T2_Train_Crab",
                                   "AnimalHusbandry_T2_Train_Daystrider",         "AnimalHusbandry_T2_Train_Hippo",                "AnimalHusbandry_T2_Train_Moonstrider_CrateMule",
                                   "AnimalHusbandry_T2_Train_Scorpion",           "AnimalHusbandry_T2_Train_Shoalskipper",         "AnimalHusbandry_T2_Train_VerdantVoyager"};
/// The Riverlands' 17 hunting kinds, as the service spawns them.
const char* const kRiverlands[17] = {"Hunting_Wolf", "Hunting_Bear", "Hunting_Rabid", "Hunting_SilkwormLarvae", "Hunting_Otter",      "Hunting_Horse", "Hunting_Raven",
                                     "Hunting_Spider", "Hunting_Bullywog", "Hunting_PallasCat", "Hunting_Gryphon", "Hunting_Deer", "Hunting_Bork", "Hunting_Tidesnapper",
                                     "Hunting_Skinwalker", "Hunting_Swiftclaw", "Hunting_TerrorBird"};
/// The 11 Journeyman recipes, only in the Journeyman station's set.
const char* const kJourneyman[11] = {"AnimalHusbandry_T3_Livestock_Baby_Lamb", "AnimalHusbandry_T3_Livestock_Baby_Piglet", "AnimalHusbandry_T3_Train_Alligator",
                                     "AnimalHusbandry_T3_Train_AlpineBaghorn_CrateMule", "AnimalHusbandry_T3_Train_Basilisk", "AnimalHusbandry_T3_Train_Bonestrider ",
                                     "AnimalHusbandry_T3_Train_Crocodile", "AnimalHusbandry_T3_Train_Iguana", "AnimalHusbandry_T3_Train_ProtectorsPride",
                                     "AnimalHusbandry_T3_Train_Scorpix", "AnimalHusbandry_T3_Train_TerrorBird"};
}  // namespace ah

/// Whether a recipe's record name is one the game still makes (not a RETIRED or NOTUSED row).
static bool ah_live_name(const std::string& name) { return name.rfind("NOTUSED_", 0) != 0 && name.rfind("OBSOLETE_", 0) != 0; }

/// The bag and the request that start `recipe` for `quantity` units from the item each input names, at the quality the recipe needs.
struct AhStart {
    repl::StartCraft                req;
    std::vector<svc::InventoryItem> items;
};

static AhStart ah_start(const content::CraftRecipeData& data, const content::CraftRecipe& recipe, const PlayerWorld& world, int quantity) {
    (void)data;
    AhStart s;
    s.req = start_of(recipe, quantity);
    int slot = 0;
    for (const content::CraftInput& in : recipe.inputs) {
        if (in.quantity < 1 || in.item == 0) continue;
        const int need = in.quantity * quantity;
        const int q = quality_at_least(world, in.item, in.min_rarity);
        s.items.push_back(farm_item(slot++, in.item, need));
        s.items.back().quality = q;
        if (!in.primary) s.req.general.push_back(resource(in.item, need, world.item_rarity_facts(in.item).rarity_of(q)));
    }
    return s;
}

/// The facts an Animal Husbandry start reads: the station, the player's tier, the bags and the station's own tax; `taught` says whether a learnable recipe is known.
static crafting::StartFacts ah_facts(const content::CraftRecipeData& data, const content::CraftStationDef& station, const items::Inventory& bag, const PlayerWorld& world, int tier,
                                     bool taught = false) {
    crafting::StartFacts f;
    f.recipes = &data;
    f.station = &station;
    f.tier_of = [tier](int64_t) { return tier; };
    f.taught = [taught](int64_t) { return taught; };
    f.inventory = &bag;
    f.rarity_facts = [&world](int64_t item) { return world.item_rarity_facts(item); };
    f.tax = crafting::station_tax(station.tax_tag);
    f.copper = 5'000'000;
    f.node.level = 3;   // a Village's Marketplace (Halcyon): the live station the recipe's lists are read at
    return f;
}

/// The items a recipe's reward table names: the mount, the beast or the livestock it makes.
static std::set<int64_t> ah_outputs(const content::CraftRecipeData& data, const content::CraftRecipe& recipe) {
    std::set<int64_t> out;
    const content::CraftReward* reward = data.reward_of(recipe);
    if (!reward) return out;
    for (const content::CraftRewardContainer& box : reward->containers) {
        for (const std::vector<content::CraftRewardItem>& units : box.rewards) {
            for (const content::CraftRewardItem& it : units) {
                if (it.item != 0) out.insert(it.item);
            }
        }
    }
    return out;
}

static std::string ah_item_name(const content::ContentDb& db, int64_t item) {
    std::string error;
    const auto rec = db.item(item, error);
    return rec ? rec->name : std::to_string(item);
}

/// The job of one unit of `recipe` made from `plan`: its products and its experience, as the claim rolls them.
static std::optional<crafting::JobRewards> ah_roll(const content::CraftRecipeData& data, const content::CraftRecipe& recipe, const crafting::Plan& plan, const PlayerWorld& world,
                                                   const char* job = "00000000000000000000000000000021") {
    const svc::ProcessingJob made = crafting::make_job({}, "0123456789abcdef", recipe.guid, 1, plan.duration_ms, {1, 2, 3, 4}, crafting::job_resources_of(plan.paid), 1000);
    svc::ProcessingJob job_copy = made;
    job_copy.job_guid = job;
    return crafting::roll_job(data, job_copy, 25, crafting::Ratings{}, [&world](int64_t item) { return world.item_rarity_facts(item); });
}

/// The station, its set and the 21 recipes: every Novice and Apprentice recipe of Animal Husbandry - Apprentice starts with its input.
static void test_husbandry_recipes(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("the Animal Husbandry - Apprentice station's 21 recipes\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    if (!loaded) {
        std::printf("  [FAIL] the recipes do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    const content::CraftRecipeData& data = *loaded;
    auto station_data = db.craft_station_data(error);
    if (!station_data) {
        std::printf("  [FAIL] the station definitions do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    const content::CraftStationDef* t1 = station_named(*station_data, "ProcessingStation_AnimalHusbandry_T1");
    const content::CraftStationDef* t2 = station_named(*station_data, "ProcessingStation_AnimalHusbandry_T2");
    const content::CraftStationDef* t3 = station_named(*station_data, "ProcessingStation_AnimalHusbandry_T3");
    CHECK_SHOWN(t2 && t2->display_name == "Animal Husbandry - Apprentice" && t2->max_tier == 1 && t2->queue_size == 2,
                "the live station is Animal Husbandry - Apprentice: tier 1 at most and a queue of 2");
    CHECK_SHOWN(t1 && t1->display_name == "Animal Husbandry - Novice" && t1->max_tier == 0 && t1->queue_size == 1,
                "the Novice station is tier 0 at most with a queue of 1");
    if (!t1 || !t2 || !t3) return;

    std::vector<const content::CraftRecipe*> stationed;
    int novices = 0, apprentices = 0;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != ah::kHusbandry || !data.set_holds(t2->recipe_set, guid)) continue;
        stationed.push_back(&r);
        if (r.tier == 0) ++novices;
        if (r.tier == 1) ++apprentices;
    }
    CHECK_SHOWN(stationed.size() == 21 && novices == 6 && apprentices == 15, "Animal Husbandry - Apprentice holds 21 recipes: 6 Novice and 15 Apprentice");
    size_t learnable = 0;
    for (const content::CraftRecipe* r : stationed) learnable += r->learnable ? 1 : 0;
    CHECK_SHOWN(learnable == 0, "none of the 21 has to be learned: Animal Husbandry is not taught by recipe items");

    // the Apprentice list's own availability row: the data's cCount placeholder, the enum's count and not a reason
    size_t lists_with_a_predicate = 0;
    std::string predicate_type;
    if (const auto lists = data.set_lists.find(t2->recipe_set); lists != data.set_lists.end()) {
        for (const int64_t list : lists->second) {
            const auto p = data.list_preds.find(list);
            if (p == data.list_preds.end()) continue;
            ++lists_with_a_predicate;
            for (const content::CraftPred& pred : p->second) predicate_type = pred.type;
        }
    }
    CHECK_SHOWN(lists_with_a_predicate == 1 && predicate_type == "cCount",
                "the Apprentice list carries one availability row, the data's cCount placeholder, which names no condition");

    // each of the 21 starts with its input, runs its base time and pays its experience and its product
    const auto tax = cr::station_tax(t2->tax_tag);
    size_t started = 0, timed = 0, costed = 0, paid = 0, made = 0;
    for (const content::CraftRecipe* r : stationed) {
        AhStart start = ah_start(data, *r, world, 1);
        auto bag = bag_of(start.items);
        if (!bag) {
            std::printf("  [FAIL] %s: its bag does not read\n", r->name.c_str());
            ++g_fail;
            continue;
        }
        const cr::Plan plan = cr::plan_start(start.req, ah_facts(data, *t2, *bag, world, std::max(1, r->tier)));
        if (!plan.ok) {
            std::printf("  [FAIL] %s does not start: %s\n", r->name.c_str(), plan.refusal.c_str());
            ++g_fail;
            continue;
        }
        ++started;
        const int64_t want_ms = static_cast<int64_t>(std::llround(r->base_duration * 1000.0));
        if (plan.duration_ms == want_ms && plan.takes.size() == 1) {
            ++timed;
        } else {
            std::printf("  [FAIL] %s: %lld ms (want %lld), %zu take(s)\n", r->name.c_str(), static_cast<long long>(plan.duration_ms), static_cast<long long>(want_ms), plan.takes.size());
            ++g_fail;
        }
        const int64_t unit = r->tier == 0 ? 5000 : 15000;
        const int64_t taxed = static_cast<int64_t>(std::floor(static_cast<double>(unit) * (1.0 + tax.percent) + tax.flat + 0.5));
        if (plan.unit_cost == unit && plan.cost == taxed && plan.cost > 0) {
            ++costed;
        } else {
            std::printf("  [FAIL] %s: %lld copper a unit, %lld for one (want %lld and %lld)\n", r->name.c_str(), static_cast<long long>(plan.unit_cost),
                        static_cast<long long>(plan.cost), static_cast<long long>(unit), static_cast<long long>(taxed));
            ++g_fail;
        }
        const auto rolled = ah_roll(data, *r, plan, world);
        const std::set<int64_t> outputs = ah_outputs(data, *r);
        const bool product_ok = rolled && rolled->profession == ah::kHusbandry && rolled->profession_xp == (r->tier == 0 ? 500 : 600) && !rolled->products.empty() && !outputs.empty() &&
                                outputs.count(rolled->products.front().item) != 0;
        if (product_ok) {
            ++paid;
            if (rolled->products.front().quantity >= 1) ++made;
        } else {
            std::printf("  [FAIL] %s: %lld experience, %zu product(s) (want %d and one of the recipe's)\n", r->name.c_str(),
                        rolled ? static_cast<long long>(rolled->profession_xp) : -1ll, rolled ? rolled->products.size() : 0, r->tier == 0 ? 500 : 600);
            ++g_fail;
        }
    }
    CHECK_SHOWN(started == 21, "all 21 Novice and Apprentice recipes start at Animal Husbandry - Apprentice with the item their input names");
    CHECK_SHOWN(timed == 21, "each job takes its input and runs its recipe's own base time (1,800 s a trainable, 60 s a livestock recipe)");
    CHECK_SHOWN(costed == 21, "each job costs its recipe's price with the station's tax (5,000 copper a Novice unit, 15,000 an Apprentice one, 50 % on top)");
    CHECK_SHOWN(paid == 21 && made == 21, "each job pays 500 Novice or 600 Apprentice experience a unit and makes one of the recipe's products");

    // the station's sets and the 33 live recipes
    size_t t1_live = 0, t3_live = 0, t3_above = 0;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != ah::kHusbandry) continue;
        t1_live += data.set_holds(t1->recipe_set, guid) ? 1 : 0;
        if (data.set_holds(t3->recipe_set, guid)) {
            ++t3_live;
            t3_above += data.set_holds(t2->recipe_set, guid) ? 0 : 1;
        }
    }
    CHECK_SHOWN(t1_live == 7 && t3_live == 32 && t3->max_tier == 2, "the Novice station holds 7 recipes and the Journeyman station (tier 2) 32");
    CHECK_SHOWN(t3_above == 11, "the 11 recipes above Apprentice are only in the Journeyman station's set (held back with the Agricultural Supply building)");
    size_t journeyman_named = 0;
    for (const char* name : ah::kJourneyman) {
        const content::CraftRecipe* r = recipe_named(data, name);
        journeyman_named += r && data.set_holds(t3->recipe_set, r->guid) && !data.set_holds(t2->recipe_set, r->guid) ? 1 : 0;
    }
    CHECK_SHOWN(journeyman_named == 11, "the Journeyman station's 11 recipes are the ones the milestone holds back, none of them at the served station");
    size_t live = 0, retired = 0, n0 = 0, n1 = 0, n2 = 0;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != ah::kHusbandry) continue;
        if (!ah_live_name(r.name)) {
            ++retired;
            continue;
        }
        ++live;
        n0 += r.tier == 0 ? 1 : 0;
        n1 += r.tier == 1 ? 1 : 0;
        n2 += r.tier == 2 ? 1 : 0;
    }
    CHECK_SHOWN(live == 33 && retired == 58 && n0 == 6 && n1 == 16 && n2 == 11, "the data's 91 recipes: 58 retired and 33 live, 6 Novice, 16 Apprentice and 11 Journeyman");
    const content::CraftRecipe* bullywog = recipe_named(data, "AnimalHusbandry_T2_Train_GiantBullywog");
    bool stationed_bullywog = false;
    for (const content::CraftStationDef* s : {t1, t2, t3}) stationed_bullywog = stationed_bullywog || (bullywog && data.set_holds(s->recipe_set, bullywog->guid));
    CHECK_SHOWN(bullywog && !stationed_bullywog && bullywog->inputs.size() == 1 && bullywog->inputs.front().item != 0,
                "AnimalHusbandry_T2_Train_GiantBullywog is at no station and takes a retired item");
    SAYOK("the Animal Husbandry - Apprentice station's 21 recipes");
}

/// The 9 recipes the Riverlands' hunts feed, the 12 they do not, and the rule at their source: the hunts and the open shops.
static void test_husbandry_sources(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("the Animal Husbandry recipes' sources\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    auto station_data = db.craft_station_data(error);
    auto shops_data = db.vendor_data(error);
    if (!loaded || !station_data || !shops_data) {
        std::printf("  [FAIL] the recipes, the stations or the shops do not read\n");
        ++g_fail;
        return;
    }
    const content::CraftRecipeData& data = *loaded;
    const content::CraftStationDef* t2 = station_named(*station_data, "ProcessingStation_AnimalHusbandry_T2");
    if (!t2) {
        std::printf("  [FAIL] Animal Husbandry - Apprentice is not in the data\n");
        ++g_fail;
        return;
    }
    const vendor::TownShops shops(std::move(*shops_data));
    std::set<int64_t> open_items;   // every item an open shop lists
    for (const int64_t shop : shops.open_shops()) {
        const auto prices = shops.data().prices.find(shop);
        if (prices == shops.data().prices.end()) continue;
        for (const auto& priced : prices->second) open_items.insert(priced.first);
    }
    size_t from_hunt = 0, no_source = 0, sold = 0;
    for (const char* name : ah::kFromHunt) {
        const content::CraftRecipe* r = recipe_named(data, name);
        if (!r) {
            std::printf("  [FAIL] %s is not in the data\n", name);
            ++g_fail;
            continue;
        }
        AhStart start = ah_start(data, *r, world, 1);
        auto bag = bag_of(start.items);
        const cr::Plan plan = bag ? cr::plan_start(start.req, ah_facts(data, *t2, *bag, world, std::max(1, r->tier))) : cr::Plan{};
        if (plan.ok && r->inputs.size() == 1) {
            ++from_hunt;
        } else {
            std::printf("  [FAIL] %s does not start from its hunt's item: %s\n", name, plan.refusal.c_str());
            ++g_fail;
        }
    }
    for (const char* name : ah::kNoSource) {
        const content::CraftRecipe* r = recipe_named(data, name);
        if (!r) {
            std::printf("  [FAIL] %s is not in the data\n", name);
            ++g_fail;
            continue;
        }
        bool any_open = false;
        for (const content::CraftInput& in : r->inputs) any_open = any_open || open_items.count(in.item) != 0;
        sold += any_open ? 1 : 0;
        ++no_source;
    }
    CHECK_SHOWN(from_hunt == 9, "9 of the 21 recipes start from a trainable the Riverlands' 17 hunting kinds give");
    CHECK_SHOWN(no_source == 12 && sold == 0, "the other 12 take an item no open shop sells: their sources are held back (the two crate mules' vendors, the hunting kinds outside the Riverlands, the livestock chain, two with no source at all)");

    // the two livestock recipes' circle: the Farming recipe that makes the adult takes the baby the Animal Husbandry recipe makes
    size_t circles = 0;
    for (const char* name : {"AnimalHusbandry_T2_Livestock_Baby_Chicken", "AnimalHusbandry_T2_Livestock_Baby_Cow"}) {
        const content::CraftRecipe* baby = recipe_named(data, name);
        const std::string adult_name = std::string(name).find("Chicken") != std::string::npos ? "Farming_T2_Livestock_Adult_Chicken" : "Farming_T2_Livestock_Adult_Cow";
        const content::CraftRecipe* adult = recipe_named(data, adult_name.c_str());
        const std::set<int64_t> babies = baby ? ah_outputs(data, *baby) : std::set<int64_t>{};
        bool circular = baby && adult && !babies.empty();
        if (circular) {
            bool takes_the_baby = false;
            for (const content::CraftInput& in : adult->inputs) takes_the_baby = takes_the_baby || babies.count(in.item) != 0;
            circular = takes_the_baby;
        }
        circles += circular ? 1 : 0;
    }
    CHECK_SHOWN(circles == 2, "the two livestock recipes' adult is made by a Farming recipe that takes the baby they make: no seed exists in the live world");

    // the five Apprentice hunts need an Apprentice Hunting Bow (the data's bows that reach tier 1 or more, worn in the bow slot): no open shop sells one; Carpentry's recipes make them and deliver
    // each into the equipment slot (the quick make serves that destination since the Weaponsmithing milestone), and their reagent is sold by the Woodshop vendor of the artisan outposts
    const auto gather = db.gather_data(error);
    std::set<int64_t> bows;
    if (gather) {
        for (const auto& [item, tool] : gather->tools) {
            if (tool.tool_slot == 38 && tool.cert_tier >= 1) bows.insert(item);
        }
    }
    size_t bows_sold = 0, bow_recipes = 0, bow_recipes_served = 0, bow_reagents_sold = 0;
    for (const int64_t bow : bows) bows_sold += open_items.count(bow) != 0 ? 1 : 0;
    for (const auto& [guid, r] : data.recipes) {
        const std::set<int64_t> made = ah_outputs(data, r);
        const bool makes_a_bow = std::any_of(bows.begin(), bows.end(), [&](int64_t bow) { return made.count(bow) != 0; });
        bow_recipes += makes_a_bow ? 1 : 0;
        bow_recipes_served += makes_a_bow && r.inventory_destination ? 1 : 0;
        if (!makes_a_bow) continue;
        for (const content::CraftInput& in : r.inputs) {
            if (in.primary && in.item != 0 && open_items.count(in.item) != 0) {
                ++bow_reagents_sold;
                break;
            }
        }
    }
    CHECK_SHOWN(gather.has_value() && !bows.empty() && bows_sold == 0 && bow_recipes > 0 && bow_recipes_served == 0 && bow_reagents_sold == bow_recipes,
                "no open shop sells a hunting bow of Apprentice or more; every recipe that makes one delivers it into the equipment slot (served by the quick make) and takes a reagent an open shop sells");
    SAYOK("the Animal Husbandry recipes' sources");
}

/// The 31 mounts, the 21 beasts and the 4 livestock; the mount item a job gives is summoned by the mount code and ridden.
static void test_husbandry_outputs(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("the products of the Animal Husbandry recipes\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    auto station_data = db.craft_station_data(error);
    if (!loaded || !station_data) {
        std::printf("  [FAIL] the recipes or the stations do not read\n");
        ++g_fail;
        return;
    }
    const content::CraftRecipeData& data = *loaded;
    const content::CraftStationDef* t2 = station_named(*station_data, "ProcessingStation_AnimalHusbandry_T2");
    if (!t2) {
        std::printf("  [FAIL] Animal Husbandry - Apprentice is not in the data\n");
        ++g_fail;
        return;
    }
    std::set<int64_t> distinct;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != ah::kHusbandry || !ah_live_name(r.name)) continue;
        for (const int64_t item : ah_outputs(data, r)) distinct.insert(item);
    }
    size_t mounts = 0, beasts = 0, livestock = 0, retired_mounts = 0;
    for (const int64_t item : distinct) {
        const std::string name = ah_item_name(db, item);
        const content::MountRow* row = world.mounts() ? world.mounts()->find(item) : nullptr;
        if (row) {
            ++mounts;
            const bool summon = row->activation == "SummonMount" && row->cast_ability != 0 && world.mounts()->summon_ability() != 0 && row->cast_ability == world.mounts()->summon_ability();
            if (!summon) {
                std::printf("  [FAIL] %s is in the mount table without SummonMount's ability\n", name.c_str());
                ++g_fail;
            }
        } else if (name.find("BeastOfBurden") != std::string::npos) {
            ++beasts;
        } else if (name.rfind("Livestock", 0) == 0 || name.rfind("Resource_Raw_Livestock", 0) == 0) {
            ++livestock;
        } else if (name.rfind("RETIRED_", 0) == 0) {
            ++retired_mounts;
        } else {
            std::printf("  [FAIL] %s is none of a mount, a beast, livestock or a retired mount\n", name.c_str());
            ++g_fail;
        }
    }
    CHECK_SHOWN(distinct.size() == 58 && mounts == 31 && beasts == 21 && livestock == 4 && retired_mounts == 2,
                "the 33 live recipes name 58 items: 31 mounts (all in the mount table with SummonMount), 21 beasts of burden, 4 livestock and 2 retired mounts");

    // a job's product goes to the Backpack, and the mount code makes the mount its item names
    const content::CraftRecipe* scorpid = recipe_named(data, "AnimalHusbandry_T1_Train_ScorpionBroodling");
    CHECK_SHOWN(scorpid != nullptr, "Train Scorpion Broodling is in the data");
    if (!scorpid) return;
    AhStart start = ah_start(data, *scorpid, world, 1);
    auto bag = bag_of(start.items);
    const cr::Plan plan = (bag && t2) ? cr::plan_start(start.req, ah_facts(data, *t2, *bag, world, 1)) : cr::Plan{};
    const auto rolled = plan.ok ? ah_roll(data, *scorpid, plan, world) : std::nullopt;
    CHECK_SHOWN(rolled && rolled->products.size() == 1, "a finished Scorpion Broodling job makes its one mount product");
    if (!rolled || rolled->products.empty() || !world.mounts()) return;
    const int64_t product = rolled->products.front().item;
    const content::MountRow* row = world.mounts()->find(product);
    CHECK_SHOWN(row && row->activation == "SummonMount" && row->cast_ability == world.mounts()->summon_ability() && !row->blueprint.empty() && !world.mounts()->rider_anim_of(*row).empty(),
                "the mount item the job gives is summoned by SummonMount and names the Blueprint and the rider animation the mount code rides");
    auto empty = bag_of({});
    if (empty) {
        svc::InventoryItem it;
        it.container = svc::ItemContainer::Backpack;
        it.item_record_id = product;
        it.instance_guid = "00000000000000000000000000000201";
        it.quantity = 1;
        it.quality = rolled->products.front().quality > 0 ? rolled->products.front().quality : rolled->quality;
        it.created_at_ms = 1;
        const items::Outcome granted = world.item_catalog() ? empty->grant({it}, *world.item_catalog()) : items::Outcome{};
        CHECK_SHOWN(granted.ok && empty->count_of(product) == 1, "the product is a Backpack item, as the claim grants it");
    }
    if (row) {
        spawns::MountParams p;
        p.player_id = 5;
        p.owner_guid = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
        p.owner_name = "Cyb";
        p.owner_level = 25;
        p.server_id = 60;
        p.x = -777664.5;
        p.y = 616578.7;
        p.z = 15952;
        p.yaw_degrees = 165.5;
        uint64_t next = 0x9000000;
        const auto minter = [&next]() {
            aoc::protocol::emit::FIntrepidNetworkGUID g;
            g.ObjectId = next;
            next += 2;
            return g;
        };
        const auto made = world.mounts()->make(*row, p, minter, error);
        CHECK_SHOWN(made.has_value(), "the mount code makes the actor of the mount item the job gave");
        if (!made) std::printf("    %s\n", error.c_str());
    }
    const content::MountRow* terror = nullptr;
    for (const int64_t item : distinct) {
        if (ah_item_name(db, item) == "Mount_Grounded_TerrorBird_0") terror = world.mounts() ? world.mounts()->find(item) : nullptr;
    }
    CHECK_SHOWN(terror != nullptr, "Animal Husbandry makes Mount_Grounded_TerrorBird_0, the mount the riding tests ride");
    SAYOK("the products of the Animal Husbandry recipes");
}

/// The trainable drops: each of the 9 comes from its Riverlands hunt, at the chance the reward table's weights give.
static void test_husbandry_hunts(const content::ContentDb& db, PlayerWorld& world) {
    std::printf("the trainable drops of the Riverlands' hunts\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    if (!loaded) {
        std::printf("  [FAIL] the recipes do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    const content::CraftRecipeData& data = *loaded;
    if (!world.gathering() && !world.start_gathering(gather::Config{}, error)) {
        std::printf("  [FAIL] gathering does not start: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    gather::Gathering* g = world.gathering();
    CHECK_SHOWN(g != nullptr && g->hunting().size() == 53, "the 53 hunting kinds are loaded");
    if (!g) return;
    size_t present = 0;
    for (const char* name : ah::kRiverlands) {
        for (const auto& [def, kind] : g->hunting()) present += kind.name == name ? 1 : 0;
    }
    CHECK_SHOWN(present == 17, "the Riverlands' 17 hunting kinds are all in the data");
    // ... and all 17 spawn in the Riverlands: the population the oracle counted (worldd's default, the zones on) names a hunting creature by its Blueprint, a Bear's Hunting_BrewBear and the Terror
    // Bird's Hunting_Terrorbird apart from their kinds' spellings
    size_t spawn_blueprints = 0, spawned_kinds = 0;
    const auto spawned_in_the_riverlands = [](const std::string& blueprint) {
        for (size_t i = 0; i < tests::kGoldenZoi.blueprint_count; ++i) {
            if (blueprint == tests::kGoldenZoi.blueprints[i].name && tests::kGoldenZoi.blueprints[i].value > 0) return true;
        }
        return false;
    };
    for (size_t i = 0; i < tests::kGoldenZoi.blueprint_count; ++i) spawn_blueprints += std::string(tests::kGoldenZoi.blueprints[i].name).rfind("Hunting_", 0) == 0 ? 1 : 0;
    for (const char* name : ah::kRiverlands) {
        std::string blueprint = name;
        if (blueprint == "Hunting_Bear") blueprint = "Hunting_BrewBear";
        if (blueprint == "Hunting_TerrorBird") blueprint = "Hunting_Terrorbird";
        spawned_kinds += spawned_in_the_riverlands(blueprint) ? 1 : 0;
    }
    CHECK_SHOWN(spawn_blueprints == 17 && spawned_kinds == 17, "the population golden of the Riverlands spawns exactly those 17 hunting kinds, so the sources the recipes name are the live region's");
    const bootstrap::StatContext ctx = world.stat_context();
    auto owner = ctx.bundle && ctx.gear ? std::make_unique<stats::CharacterStats>(*ctx.bundle, *ctx.gear, 2, 25, ctx.rounding) : nullptr;
    std::string why;
    if (!owner || !owner->start({}, why)) {
        std::printf("  [FAIL] the stats do not start: %s\n", why.c_str());
        ++g_fail;
        return;
    }
    auto run = owner->evaluation({}, why);
    if (!run) {
        std::printf("  [FAIL] the stat run does not start: %s\n", why.c_str());
        ++g_fail;
        return;
    }
    g->set_ratings({});
    size_t drops = 0, sure = 0, chance = 0, tiered = 0, in_band = 0;
    for (const ah::HuntDrop& want : ah::kDrop) {
        const content::CraftRecipe* r = recipe_named(data, want.recipe);
        const gather::Gathering::HuntKind* kind = nullptr;
        for (const auto& [def, k] : g->hunting()) {
            if (k.name == want.hunt) kind = &k;
        }
        const int64_t item = r && !r->inputs.empty() ? r->inputs.front().item : 0;
        if (!r || !kind || item == 0) {
            std::printf("  [FAIL] %s or its hunt %s is not in the data\n", want.recipe, want.hunt);
            ++g_fail;
            continue;
        }
        // the hunt asks for the certification the recipe's own tier names: the four Novice recipes' animals are hunted with the Crude Hunting Bow, the five Apprentice ones need Apprentice
        // Hunting and an Apprentice bow
        if (kind->row && kind->row->cert_tier == r->tier && kind->row->tool_slot == 38) ++tiered;
        stats::SplitMix rng;
        rng.state = 0x48554E54ull ^ static_cast<uint64_t>(want.percent);
        size_t got = 0;
        for (int i = 0; i < 300; ++i) {
            const auto h = g->harvest(*kind->row, *run, 25, world::NodeSite{}, nullptr, rng, why);
            for (const combat::RolledItem& it : h ? h->items : std::vector<combat::RolledItem>{}) got += (static_cast<int64_t>(it.item) == item) ? 1 : 0;
        }
        // the chance the reward table's weights give: every roll, or one in five (300 rolls of one seed: 60 expected, the band is about four standard deviations wide)
        if (want.percent == 100 ? got == 300 : (got >= 30 && got <= 90)) ++in_band;
        if (want.percent == 100 ? got == 300 : (got > 0 && got < 300)) {
            ++drops;
            if (want.percent == 100) ++sure;
            else ++chance;
        } else {
            std::printf("  [FAIL] %s: %zu of 300 rolls gave its trainable (the table's chance is %d %%)\n", want.recipe, got, want.percent);
            ++g_fail;
        }
    }
    CHECK_SHOWN(drops == 9 && sure == 4 && chance == 5,
                "each of the 9 trainables drops from its Riverlands hunt: 4 kinds give it on every roll, 5 on a part of them (the tables' 20 %)");
    CHECK_SHOWN(in_band == 9, "the five on a part of the rolls give it about one roll in five, the other four on every roll");
    CHECK_SHOWN(tiered == 9, "the four Novice animals are hunted at Novice and the five Apprentice ones (Deer, Raptor, Skinwalker, Tidesnapper, Otter) at Apprentice, all with the bow slot");
    SAYOK("the trainable drops of the Riverlands' hunts");
}

/// The chain the Cooking recipes wait on: a baby, the Farming adult it becomes, the egg or the milk, and the Cooking recipe that takes it.
static void test_husbandry_livestock_chain(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("the livestock chain's produce\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    auto station_data = db.craft_station_data(error);
    if (!loaded || !station_data) {
        std::printf("  [FAIL] the recipes or the stations do not read\n");
        ++g_fail;
        return;
    }
    const content::CraftRecipeData& data = *loaded;
    const content::CraftStationDef* ah2 = station_named(*station_data, "ProcessingStation_AnimalHusbandry_T2");
    const content::CraftStationDef* farm2 = station_named(*station_data, "ProcessingStation_Farming_T2");
    const content::CraftStationDef* cook2 = station_named(*station_data, "ProcessingStation_Cooking_T2");
    if (!ah2 || !farm2 || !cook2) {
        std::printf("  [FAIL] a station of the chain is not in the data\n");
        ++g_fail;
        return;
    }
    struct Link {
        const char* baby;
        const char* adult;
        const char* produce;
        const char* cooking;
    };
    const Link links[2] = {{"AnimalHusbandry_T2_Livestock_Baby_Chicken", "Farming_T2_Livestock_Adult_Chicken", "Farming_T2_Egg_Chicken", "Cooking_T2_Food_ScrambledEggs"},
                           {"AnimalHusbandry_T2_Livestock_Baby_Cow", "Farming_T2_Livestock_Adult_Cow", "Farming_T2_Drink_CowsMilk", "Cooking_T2_Ingredient_Butter"}};
    // an input of a recipe, named by any of its resources (the reagent is listed first, so the order is not the recipe's request order)
    const auto takes_item = [](const content::CraftRecipe& r, int64_t item) {
        for (const content::CraftInput& in : r.inputs) {
            if (in.item == item) return true;
        }
        return false;
    };
    size_t chains = 0;
    for (const Link& link : links) {
        const content::CraftRecipe* baby = recipe_named(data, link.baby);
        const content::CraftRecipe* adult = recipe_named(data, link.adult);
        const content::CraftRecipe* produce = recipe_named(data, link.produce);
        const content::CraftRecipe* cooking = recipe_named(data, link.cooking);
        if (!baby || !adult || !produce || !cooking) {
            std::printf("  [FAIL] %s, %s, %s or %s is not in the data\n", link.baby, link.adult, link.produce, link.cooking);
            ++g_fail;
            continue;
        }
        AhStart baby_start = ah_start(data, *baby, world, 1);
        auto baby_bag = bag_of(baby_start.items);
        const cr::Plan baby_plan = baby_bag ? cr::plan_start(baby_start.req, ah_facts(data, *ah2, *baby_bag, world, 1)) : cr::Plan{};
        const auto baby_made = baby_plan.ok ? ah_roll(data, *baby, baby_plan, world) : std::nullopt;
        const int64_t baby_item = baby_made && !baby_made->products.empty() ? baby_made->products.front().item : 0;
        bool adult_ok = baby_item != 0 && takes_item(*adult, baby_item);
        const auto adult_made = adult_ok ? [&] {
            AhStart s = ah_start(data, *adult, world, 1);
            auto bag = bag_of(s.items);
            const cr::Plan plan = bag ? cr::plan_start(s.req, ah_facts(data, *farm2, *bag, world, 1)) : cr::Plan{};
            return plan.ok ? ah_roll(data, *adult, plan, world, "00000000000000000000000000000022") : std::nullopt;
        }() : std::nullopt;
        const int64_t adult_item = adult_made && !adult_made->products.empty() ? adult_made->products.front().item : 0;
        bool produce_ok = adult_item != 0 && takes_item(*produce, adult_item);
        const auto produce_made = produce_ok ? [&] {
            AhStart s = ah_start(data, *produce, world, 1);
            auto bag = bag_of(s.items);
            const cr::Plan plan = bag ? cr::plan_start(s.req, ah_facts(data, *farm2, *bag, world, 1)) : cr::Plan{};
            return plan.ok ? ah_roll(data, *produce, plan, world, "00000000000000000000000000000023") : std::nullopt;
        }() : std::nullopt;
        const int64_t produce_item = produce_made && !produce_made->products.empty() ? produce_made->products.front().item : 0;
        bool cooking_ok = produce_item != 0;
        if (cooking_ok) {
            bool takes = false;
            for (const content::CraftInput& in : cooking->inputs) takes = takes || in.item == produce_item;
            cooking_ok = takes;
        }
        if (cooking_ok) {
            AhStart s = ah_start(data, *cooking, world, 1);
            auto bag = bag_of(s.items);
            const cr::Plan plan = bag ? cr::plan_start(s.req, ah_facts(data, *cook2, *bag, world, 1)) : cr::Plan{};
            cooking_ok = plan.ok;
            if (!plan.ok) std::printf("    %s\n", plan.refusal.c_str());
        }
        if (baby_plan.ok && baby_item != 0 && adult_ok && adult_made && adult_item != 0 && produce_ok && produce_made && produce_item != 0 && cooking_ok) {
            ++chains;
            std::printf("    %s -> %s -> %s -> %s: the %s comes out and Cooking takes it\n", link.baby, link.adult, link.produce, link.cooking, ah_item_name(db, produce_item).c_str());
        } else {
            std::printf("  [FAIL] the chain of %s does not run: baby %lld, adult %lld, produce %lld\n", link.baby, static_cast<long long>(baby_item), static_cast<long long>(adult_item),
                        static_cast<long long>(produce_item));
            ++g_fail;
        }
    }
    CHECK_SHOWN(chains == 2, "the chicken's egg and the cow's milk come out through Farming and the Cooking recipes that wait on them start");

    // how many of the Cooking Novice and Apprentice recipes depend on a livestock baby at all: the chains of the Cooking milestone
    std::map<int64_t, std::vector<const content::CraftRecipe*>> makers;
    for (const auto& [guid, r] : data.recipes) {
        if (!ah_live_name(r.name)) continue;
        for (const int64_t item : ah_outputs(data, r)) makers[item].push_back(&r);
    }
    std::set<int64_t> babies;
    for (const Link& link : links) {
        if (const content::CraftRecipe* baby = recipe_named(data, link.baby)) {
            for (const int64_t item : ah_outputs(data, *baby)) babies.insert(item);
        }
    }
    std::function<bool(int64_t, int, std::set<int64_t>)> reaches_a_baby = [&](int64_t item, int depth, std::set<int64_t> seen) -> bool {
        if (depth <= 0 || seen.count(item) != 0) return false;
        seen.insert(item);
        const auto made = makers.find(item);
        if (made == makers.end()) return false;
        for (const content::CraftRecipe* maker : made->second) {
            for (const content::CraftInput& in : maker->inputs) {
                if (in.item == 0) continue;
                if (babies.count(in.item) != 0 || reaches_a_baby(in.item, depth - 1, seen)) return true;
            }
        }
        return false;
    };
    size_t fed = 0;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != ah::kCooking || !ah_live_name(r.name) || r.tier > 1 || !data.set_holds(cook2->recipe_set, guid)) continue;
        for (const content::CraftInput& in : r.inputs) {
            if (in.item != 0 && reaches_a_baby(in.item, 4, {})) {
                ++fed;
                break;
            }
        }
    }
    CHECK_SHOWN(fed == 28, "28 of the Cooking Novice and Apprentice recipes wait on the produce this profession's livestock chain makes");
    SAYOK("the livestock chain's produce");
}

/// The station's checks and the other order: the tier, the certification, the queue, the wrong input, the same job twice, a second player, a reconnect.
static void test_husbandry_conditions(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("the checks that decide an Animal Husbandry start\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    auto station_data = db.craft_station_data(error);
    if (!loaded || !station_data) {
        std::printf("  [FAIL] the recipes or the stations do not read\n");
        ++g_fail;
        return;
    }
    const content::CraftRecipeData& data = *loaded;
    const content::CraftStationDef* t1 = station_named(*station_data, "ProcessingStation_AnimalHusbandry_T1");
    const content::CraftStationDef* t2 = station_named(*station_data, "ProcessingStation_AnimalHusbandry_T2");
    const content::CraftRecipe* horse = recipe_named(data, "AnimalHusbandry_T1_Train_Horse");
    const content::CraftRecipe* bear = recipe_named(data, "AnimalHusbandry_T1_Train_Bear");
    const content::CraftRecipe* deer = recipe_named(data, "AnimalHusbandry_T2_Train_Deer");
    const content::CraftRecipe* raptor = recipe_named(data, "AnimalHusbandry_T2_Train_Raptor");
    CHECK_SHOWN(t1 && t2 && horse && bear && deer && raptor, "the two stations and four recipes are in the data");
    if (!t1 || !t2 || !horse || !bear || !deer || !raptor) return;

    // the list's availability row: the cCount placeholder names no condition; another unknown type is still refused
    {
        content::CraftPred count;
        count.type = "cCount";
        count.node_level = 0;
        content::CraftPred other;
        other.type = "AnotherReason";
        cr::NodeFacts node;
        node.level = 3;
        std::string why;
        const bool count_holds = cr::predicate_holds(count, node, why);
        why.clear();
        const bool other_holds = cr::predicate_holds(other, node, why);
        CHECK_SHOWN(count_holds, "the data's cCount availability row names no condition and holds at the live station");
        CHECK_SHOWN(!other_holds && why.find("unknown condition AnotherReason") != std::string::npos, "an availability type the data does not use is still refused as an unknown condition");
    }

    {   // the certification: a Novice is refused the Apprentice recipe; the Apprentice starts it
        AhStart start = ah_start(data, *deer, world, 1);
        auto bag = bag_of(start.items);
        if (bag) {
            const cr::Plan novice = cr::plan_start(start.req, ah_facts(data, *t2, *bag, world, 0));
            const cr::Plan apprentice = cr::plan_start(start.req, ah_facts(data, *t2, *bag, world, 1));
            CHECK_SHOWN(!novice.ok && novice.refusal.find("needs certification tier 1") != std::string::npos, "a Novice's Apprentice recipe is refused for the certification");
            CHECK_SHOWN(apprentice.ok, "the Apprentice certification opens it at the same station");
        }
    }
    {   // the station's tier: the Apprentice recipe at a station that reaches tier 0; the Novice station's set does not hold it at all
        AhStart start = ah_start(data, *deer, world, 1);
        auto bag = bag_of(start.items);
        if (bag) {
            content::CraftStationDef novice_tier = *t2;
            novice_tier.max_tier = 0;
            const cr::Plan tier = cr::plan_start(start.req, ah_facts(data, novice_tier, *bag, world, 1));
            CHECK_SHOWN(!tier.ok && tier.refusal.find("Animal Husbandry - Apprentice reaches 0") != std::string::npos, "an Apprentice recipe is refused at a station that reaches tier 0");
        }
        AhStart raptor_start = ah_start(data, *raptor, world, 1);
        auto raptor_bag = bag_of(raptor_start.items);
        if (raptor_bag) {
            const cr::Plan set = cr::plan_start(raptor_start.req, ah_facts(data, *t1, *raptor_bag, world, 1));
            CHECK_SHOWN(!set.ok && set.refusal.find("is not one of the recipes of Animal Husbandry - Novice") != std::string::npos, "the Novice station's set holds no Apprentice recipe");
        }
    }
    {   // the wrong input: the Horse recipe refuses a bag of a Bear trainable, and starts with the horse
        AhStart wrong = ah_start(data, *bear, world, 1);
        auto wrong_bag = bag_of(wrong.items);
        repl::StartCraft horse_req = start_of(*horse, 1);
        if (wrong_bag && !wrong.items.empty()) {
            horse_req.general = {resource(wrong.items.front().item_record_id, 1)};
            const cr::Plan plan = cr::plan_start(horse_req, ah_facts(data, *t2, *wrong_bag, world, 1));
            CHECK_SHOWN(!plan.ok && plan.refusal.find("does not name item") != std::string::npos, "the Horse recipe refuses the Bear trainable the request names");
            AhStart right = ah_start(data, *horse, world, 1);
            auto right_bag = bag_of(right.items);
            const cr::Plan ok = right_bag ? cr::plan_start(right.req, ah_facts(data, *t2, *right_bag, world, 1)) : cr::Plan{};
            CHECK_SHOWN(ok.ok, "the same recipe starts with the horse's trainable");
        }
    }
    {   // the same job twice and a second player at the same station: planning takes nothing; the first job's takes empty the bag
        AhStart start = ah_start(data, *horse, world, 1);
        auto bag = bag_of(start.items);
        if (bag) {
            const cr::Plan first = cr::plan_start(start.req, ah_facts(data, *t2, *bag, world, 1));
            bool applied = first.ok;
            for (const cr::Take& t : first.takes) applied = bag->take_materials(t.record, t.facts, t.rarity_lo, t.rarity_hi, t.count, t.lowest_first).ok && applied;
            const cr::Plan again = cr::plan_start(start.req, ah_facts(data, *t2, *bag, world, 1));
            CHECK_SHOWN(applied && first.ok && !again.ok, "the same job started twice: the second finds the materials the first took gone");
            AhStart other = ah_start(data, *horse, world, 1);
            auto other_bag = bag_of(other.items);
            const cr::Plan second_player = other_bag ? cr::plan_start(other.req, ah_facts(data, *t2, *other_bag, world, 1)) : cr::Plan{};
            CHECK_SHOWN(second_player.ok, "a second player with his own trainable starts the same job at the same station");
            const std::vector<svc::JobResource> saved = cr::job_resources_of(first.paid);
            const svc::ProcessingJob job = cr::make_job({}, "0123456789abcdef", horse->guid, 1, first.duration_ms, {1, 2, 3, 4}, saved, 1000);
            const auto running = cr::to_state(job, 30'000, 100.0);
            const auto later = cr::to_state(job, 45'000, 500.0);
            const auto done = cr::status_at(job, 1'801'000) == cr::kComplete;
            CHECK_SHOWN(running && running->status == cr::kInProgress && running->general.size() == 1 && later && later->quantity == 1 && done,
                        "a reconnect mid-job shows the same horse job, its item and its time left, and it completes after 1,800 s");
        }
    }
    {   // the queue and the quantity: the Apprentice station's two slots, a third job refused; a 250-unit job of the Novice recipe
        AhStart start = ah_start(data, *horse, world, 1);
        auto bag = bag_of(start.items);
        if (bag) {
            cr::StartFacts full = ah_facts(data, *t2, *bag, world, 1);
            full.unfinished_here = static_cast<size_t>(std::max(1, t2->queue_size));
            const cr::Plan queued = cr::plan_start(start.req, full);
            CHECK_SHOWN(!queued.ok && queued.refusal.find("the queue at Animal Husbandry - Apprentice is full (2 of 2)") != std::string::npos,
                        "a third job at the station's two-slot queue is refused");
        }
        AhStart many = ah_start(data, *horse, world, 250);
        auto many_bag = bag_of(many.items);
        if (many_bag) {
            const cr::Plan plan = cr::plan_start(many.req, ah_facts(data, *t2, *many_bag, world, 1));
            CHECK_SHOWN(plan.ok && plan.takes.size() == 1 && plan.duration_ms == 450'000'000, "a 250-unit Novice job starts, takes 250 trainables and runs 125 h");
        }
    }
    SAYOK("the checks that decide an Animal Husbandry start");
}

/// A Novice (certification tier 0) at both stations: the six Novice recipes start at Animal Husbandry - Apprentice and at Animal Husbandry - Novice (a queue of 1), and Train Deer, which the
/// Novice station's set holds, is refused there for its tier.
static void test_husbandry_novice(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("the Animal Husbandry Novice recipes at their two stations\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    auto station_data = db.craft_station_data(error);
    if (!loaded || !station_data) {
        std::printf("  [FAIL] the recipes or the stations do not read\n");
        ++g_fail;
        return;
    }
    const content::CraftRecipeData& data = *loaded;
    const content::CraftStationDef* t1 = station_named(*station_data, "ProcessingStation_AnimalHusbandry_T1");
    const content::CraftStationDef* t2 = station_named(*station_data, "ProcessingStation_AnimalHusbandry_T2");
    const content::CraftRecipe* deer = recipe_named(data, "AnimalHusbandry_T2_Train_Deer");
    const content::CraftRecipe* horse = recipe_named(data, "AnimalHusbandry_T1_Train_Horse");
    if (!t1 || !t2 || !deer || !horse) {
        std::printf("  [FAIL] a station or a recipe is not in the data\n");
        ++g_fail;
        return;
    }
    size_t novices = 0, at_apprentice_station = 0, at_novice_station = 0;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != ah::kHusbandry || r.tier != 0 || !ah_live_name(r.name)) continue;
        ++novices;
        AhStart start = ah_start(data, r, world, 1);
        auto bag = bag_of(start.items);
        if (!bag) continue;
        const cr::Plan upper = cr::plan_start(start.req, ah_facts(data, *t2, *bag, world, 0));
        const cr::Plan lower = cr::plan_start(start.req, ah_facts(data, *t1, *bag, world, 0));
        if (upper.ok && upper.cost == 7500) {
            ++at_apprentice_station;
        } else {
            std::printf("  [FAIL] %s does not start for a Novice at Animal Husbandry - Apprentice: %s\n", r.name.c_str(), upper.refusal.c_str());
            ++g_fail;
        }
        if (lower.ok && lower.cost == 7500 && lower.duration_ms == 1'800'000) {
            ++at_novice_station;
        } else {
            std::printf("  [FAIL] %s does not start for a Novice at Animal Husbandry - Novice: %s\n", r.name.c_str(), lower.refusal.c_str());
            ++g_fail;
        }
    }
    CHECK_SHOWN(novices == 6 && at_apprentice_station == 6, "a character at Novice starts each of the 6 Novice recipes at Animal Husbandry - Apprentice for 7,500 copper");
    CHECK_SHOWN(novices == 6 && at_novice_station == 6, "the same character starts them at Animal Husbandry - Novice, which the artisan buildings carry, and each runs 1,800 s");

    AhStart start = ah_start(data, *horse, world, 1);
    auto bag = bag_of(start.items);
    if (bag) {
        crafting::StartFacts busy = ah_facts(data, *t1, *bag, world, 0);
        busy.unfinished_here = 1;
        const cr::Plan queued = cr::plan_start(start.req, busy);
        CHECK_SHOWN(!queued.ok && queued.refusal.find("the queue at Animal Husbandry - Novice is full (1 of 1)") != std::string::npos, "the Novice station's one slot is taken: a second job is refused");
    }
    AhStart deer_start = ah_start(data, *deer, world, 1);
    auto deer_bag = bag_of(deer_start.items);
    if (deer_bag) {
        const cr::Plan novice = cr::plan_start(deer_start.req, ah_facts(data, *t1, *deer_bag, world, 0));
        const cr::Plan apprentice = cr::plan_start(deer_start.req, ah_facts(data, *t1, *deer_bag, world, 1));
        CHECK_SHOWN(!novice.ok && novice.refusal.find("needs certification tier 1") != std::string::npos, "Train Deer, which the Novice station's set holds, is refused a Novice for the certification");
        CHECK_SHOWN(!apprentice.ok && apprentice.refusal.find("Animal Husbandry - Novice reaches 0") != std::string::npos,
                    "... and an Apprentice for the station's tier: only Animal Husbandry - Apprentice makes it");
    }
    SAYOK("the Animal Husbandry Novice recipes at their two stations");
}

/// What the request names: too few, none, a pick the recipe does not take, and a bag that holds less than the request names.
static void test_husbandry_picks(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("the picks of an Animal Husbandry start\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    auto station_data = db.craft_station_data(error);
    if (!loaded || !station_data) {
        std::printf("  [FAIL] the recipes or the stations do not read\n");
        ++g_fail;
        return;
    }
    const content::CraftRecipeData& data = *loaded;
    const content::CraftStationDef* t2 = station_named(*station_data, "ProcessingStation_AnimalHusbandry_T2");
    const content::CraftRecipe* chick = recipe_named(data, "AnimalHusbandry_T2_Livestock_Baby_Chicken");
    const content::CraftRecipe* horse = recipe_named(data, "AnimalHusbandry_T1_Train_Horse");
    const content::CraftRecipe* bear = recipe_named(data, "AnimalHusbandry_T1_Train_Bear");
    if (!t2 || !chick || !horse || !bear || chick->inputs.size() != 1 || chick->inputs.front().quantity != 2) {
        std::printf("  [FAIL] the station or a recipe is not in the data (the baby chicken takes 2 adults)\n");
        ++g_fail;
        return;
    }
    const auto plan_of = [&](const repl::StartCraft& req, std::vector<svc::InventoryItem> items) {
        auto bag = bag_of(std::move(items));
        return bag ? cr::plan_start(req, ah_facts(data, *t2, *bag, world, 1)) : cr::Plan{};
    };
    AhStart full = ah_start(data, *chick, world, 1);   // the bag holds the 2 adults and the request names them
    CHECK_SHOWN(plan_of(full.req, full.items).ok, "the baby chicken starts with the 2 adults it takes");

    repl::StartCraft few = full.req;   // one of the two named
    few.general.front().rarities.front().count = 1;
    const cr::Plan few_plan = plan_of(few, full.items);
    CHECK_SHOWN(!few_plan.ok && few_plan.refusal.find("the request names 1 of item") != std::string::npos && few_plan.refusal.find("takes 2 for 1") != std::string::npos,
                "too few picks: one adult named for the recipe's two is refused");

    std::vector<svc::InventoryItem> thin = full.items;   // two named, one held
    thin.front().quantity = 1;
    const cr::Plan thin_plan = plan_of(full.req, thin);
    CHECK_SHOWN(!thin_plan.ok && thin_plan.refusal.find("the bags hold 1 of item") != std::string::npos, "picks the bags do not hold: two adults named, one held, is refused");

    repl::StartCraft none = full.req;   // nothing named
    none.general.clear();
    const cr::Plan none_plan = plan_of(none, full.items);
    CHECK_SHOWN(!none_plan.ok && none_plan.refusal.find("does not name item") != std::string::npos, "no pick: a request that names nothing is refused");

    AhStart horse_start = ah_start(data, *horse, world, 1);
    repl::StartCraft extra = full.req;   // a pick the recipe does not take, besides the right ones
    extra.general.push_back(horse_start.req.general.front());
    std::vector<svc::InventoryItem> both = full.items;
    both.push_back(horse_start.items.front());
    both.back().slot = 7;   // (a stack of its own: two stacks do not share a slot)
    const cr::Plan extra_plan = plan_of(extra, both);
    CHECK_SHOWN(!extra_plan.ok && extra_plan.refusal.find("does not take") != std::string::npos, "a pick the recipe does not take, beside the right ones, is refused");

    AhStart two = ah_start(data, *horse, world, 2);   // a job of 2 units takes 2 trainables
    repl::StartCraft one_named = two.req;
    one_named.general.front().rarities.front().count = 1;
    const cr::Plan one_plan = plan_of(one_named, two.items);
    CHECK_SHOWN(!one_plan.ok && one_plan.refusal.find("takes 2 for 2") != std::string::npos, "two units of the Horse recipe with one trainable named are refused");
    CHECK_SHOWN(plan_of(two.req, two.items).ok, "... and start with both named");

    AhStart bear_start = ah_start(data, *bear, world, 1);   // the Bear trainable in the bag and named for the Horse recipe
    repl::StartCraft wrong = start_of(*horse, 1);
    wrong.general = bear_start.req.general;
    const cr::Plan wrong_plan = plan_of(wrong, bear_start.items);
    CHECK_SHOWN(!wrong_plan.ok && wrong_plan.refusal.find("does not name item") != std::string::npos, "a wrong pick: the Bear trainable for the Horse recipe is refused");
    SAYOK("the picks of an Animal Husbandry start");
}

/// Every product of the 21 recipes of the served station reaches the Backpack as the claim grants it, and every mount among them is made by the mount code.
static void test_husbandry_every_product(const content::ContentDb& db, const PlayerWorld& world) {
    std::printf("every product of the Animal Husbandry - Apprentice station's recipes\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    auto station_data = db.craft_station_data(error);
    if (!loaded || !station_data || !world.item_catalog() || !world.mounts()) {
        std::printf("  [FAIL] the recipes, the stations, the item table or the mounts do not read\n");
        ++g_fail;
        return;
    }
    const content::CraftRecipeData& data = *loaded;
    const content::CraftStationDef* t2 = station_named(*station_data, "ProcessingStation_AnimalHusbandry_T2");
    if (!t2) {
        std::printf("  [FAIL] Animal Husbandry - Apprentice is not in the data\n");
        ++g_fail;
        return;
    }
    std::set<int64_t> products;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != ah::kHusbandry || !data.set_holds(t2->recipe_set, guid)) continue;
        for (const int64_t item : ah_outputs(data, r)) products.insert(item);
    }
    const items::ItemCatalog& catalog = *world.item_catalog();
    size_t known = 0, granted = 0, mounts = 0, made = 0, ridden = 0, beasts = 0, livestock = 0, retired = 0;
    uint64_t next = 0x9100000;
    const auto minter = [&next]() {
        aoc::protocol::emit::FIntrepidNetworkGUID g;
        g.ObjectId = next;
        next += 2;
        return g;
    };
    for (const int64_t item : products) {
        const std::string name = ah_item_name(db, item);
        known += catalog.info(item) ? 1 : 0;
        auto empty = bag_of({});
        svc::InventoryItem it;
        it.container = svc::ItemContainer::Backpack;
        it.item_record_id = item;
        it.instance_guid = "00000000000000000000000000000301";
        it.quantity = 1;
        it.quality = 1;
        it.created_at_ms = 1;
        if (empty && empty->can_grant({it}, catalog)) {
            const items::Outcome o = empty->grant({it}, catalog);
            granted += o.ok && empty->count_of(item) == 1 ? 1 : 0;
        }
        if (name.find("BeastOfBurden") != std::string::npos) ++beasts;
        if (name.rfind("Livestock", 0) == 0 || name.rfind("Resource_Raw_Livestock", 0) == 0) ++livestock;
        if (name.rfind("RETIRED_", 0) == 0) ++retired;   // (Train Bork's product: its input has no source, so no job makes it)
        const content::MountRow* row = world.mounts()->find(item);
        if (!row) continue;
        ++mounts;
        spawns::MountParams p;
        p.player_id = 5;
        p.owner_guid = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
        p.owner_name = "Cyb";
        p.owner_level = 25;
        p.server_id = 60;
        p.x = -777664.5;
        p.y = 616578.7;
        p.z = 15952;
        p.yaw_degrees = 165.5;
        const auto actor = world.mounts()->make(*row, p, minter, error);
        made += actor ? 1 : 0;
        if (!actor) std::printf("  [FAIL] %s: the mount code does not make it: %s\n", name.c_str(), error.c_str());
        const bool summoned = row->activation == "SummonMount" && row->cast_ability != 0 && row->cast_ability == world.mounts()->summon_ability() && !row->blueprint.empty();
        ridden += summoned && !world.mounts()->rider_anim_of(*row).empty() ? 1 : 0;
    }
    std::printf("    %zu products: %zu mounts, %zu beasts of burden, %zu livestock, %zu retired\n", products.size(), mounts, beasts, livestock, retired);
    CHECK_SHOWN(!products.empty() && known == products.size() && granted == products.size(),
                "every product of the 21 recipes is in the item table and is granted to an empty Backpack as the claim grants it (a product the table did not know would leave its job uncollectable)");
    CHECK_SHOWN(mounts > 0 && made == mounts && ridden == mounts, "every mount among them is made by the mount code, is summoned by SummonMount and names its rider animation");
    CHECK_SHOWN(products.size() == 37 && mounts == 21 && beasts == 13 && livestock == 2 && retired == 1,
                "the 21 recipes name 37 products: 21 mounts, 13 beasts of burden, 2 livestock items and the retired mount of Train Bork");
    SAYOK("every product of the Animal Husbandry - Apprentice station's recipes");
}

/// The queue of two at the station, and what a reconnect shows of the jobs behind each other and of a finished one.
static void test_husbandry_saved_jobs(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("the Animal Husbandry jobs a reconnect shows\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    auto station_data = db.craft_station_data(error);
    if (!loaded || !station_data) {
        std::printf("  [FAIL] the recipes or the stations do not read\n");
        ++g_fail;
        return;
    }
    const content::CraftRecipeData& data = *loaded;
    const content::CraftStationDef* t2 = station_named(*station_data, "ProcessingStation_AnimalHusbandry_T2");
    const content::CraftRecipe* horse = recipe_named(data, "AnimalHusbandry_T1_Train_Horse");
    if (!t2 || !horse) {
        std::printf("  [FAIL] the station or the Horse recipe is not in the data\n");
        ++g_fail;
        return;
    }
    AhStart start = ah_start(data, *horse, world, 1);
    auto bag = bag_of(start.items);
    if (!bag) return;
    const cr::Plan plan = cr::plan_start(start.req, ah_facts(data, *t2, *bag, world, 1));
    CHECK_SHOWN(plan.ok && plan.duration_ms == 1'800'000, "the Horse job starts and runs 1,800 s");
    if (!plan.ok) return;
    constexpr const char* kKey = "0123456789abcdef";
    const std::vector<svc::JobResource> saved = cr::job_resources_of(plan.paid);
    const svc::ProcessingJob first = cr::make_job({}, kKey, horse->guid, 1, plan.duration_ms, {1, 2, 3, 4}, saved, 1000);
    const svc::ProcessingJob second = cr::make_job({first}, kKey, horse->guid, 1, plan.duration_ms, {5, 6, 7, 8}, saved, 2000);
    CHECK_SHOWN(second.started_at_ms == first.end_ms(), "the second Horse job begins when the first ends");
    const auto waiting = cr::to_state(second, 30'000, 100.0);
    CHECK_SHOWN(waiting && waiting->status == cr::kPending && waiting->general.size() == 1, "a reconnect shows the second job pending, with its own trainable");
    CHECK_SHOWN(cr::unfinished_at({first, second}, kKey, 30'000) == 2 && cr::held_at({first, second}, kKey) == 2, "both count against the station's two-slot queue");

    // a third start is refused with the two running; once the first is over there is room for it
    crafting::StartFacts two_running = ah_facts(data, *t2, *bag, world, 1);
    two_running.unfinished_here = cr::unfinished_at({first, second}, kKey, 30'000);
    two_running.held_here = cr::held_at({first, second}, kKey);
    const cr::Plan third = cr::plan_start(start.req, two_running);
    CHECK_SHOWN(!third.ok && third.refusal.find("the queue at Animal Husbandry - Apprentice is full (2 of 2)") != std::string::npos, "a third job is refused while the two run");
    crafting::StartFacts one_left = ah_facts(data, *t2, *bag, world, 1);
    one_left.unfinished_here = cr::unfinished_at({first, second}, kKey, 1'802'000);
    one_left.held_here = cr::held_at({first, second}, kKey);
    CHECK_SHOWN(one_left.unfinished_here == 1 && cr::plan_start(start.req, one_left).ok, "after the first is over the station has room again (the finished job stays held until it is collected)");
    CHECK_SHOWN(cr::unfinished_at({}, kKey, 30'000) == 0, "a second player's own jobs at the same station start from none");

    // a finished job: after a reconnect it shows its product and the experience it pays
    svc::ProcessingJob job = first;
    job.job_guid = "00000000000000000000000000000031";
    const auto rolled = cr::roll_job(data, job, 25, cr::Ratings{}, [&](int64_t item) { return world.item_rarity_facts(item); });
    CHECK_SHOWN(rolled && !rolled->products.empty() && rolled->profession == ah::kHusbandry && rolled->profession_xp == 500, "the finished Horse job makes its product and pays 500 Animal Husbandry experience");
    if (rolled) {
        const auto done = cr::to_state(job, 1'900'000, 100.0, &*rolled);
        CHECK_SHOWN(done && done->status == cr::kComplete && !done->products.empty() && done->general.size() == 1, "a reconnect after the end shows it complete, with its product and the trainable it was paid with");
    }
    SAYOK("the Animal Husbandry jobs a reconnect shows");
}
