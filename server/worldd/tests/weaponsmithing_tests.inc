// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  weaponsmithing_tests.inc: Weaponsmithing, the Equipment destination and the crafted tools (the Weaponsmithing milestone)
//
//  The real content.db: 763 Weaponsmithing recipes, 34 of them of the Equipment destination (54 in all 22 professions: the
//  gathering tools of Weaponsmithing, Carpentry, Mining and Lumberjacking), whose products are worn in the tool slot
//  (Pickaxe 26, Axe 29, Sickle 33, Fishing Rod 35, Bow 38).  A recipe whose product goes to the equipment storage is made at
//  the station that lists it and the tool goes to its tool slot when that slot is free (the bag otherwise); before the
//  milestone plan_start refused every one of them ("makes gear that is worn at once").  The stations of gathering tools are
//  served now that their recipes can be delivered: without them the Master and Grand Master tools have no home at all.
//  The counts of who starts what at the live stations, with the gather-item filter table (contentc 47, whose listed filters decide the pick) and
//  without it (an older file's shape), the Novice and Apprentice tools' plan, quality, durability and experience, and the holdback of the Smithy.
//  Included by test_worldd_professions.cpp after its helpers and the other profession files, before main().
// ============================================================================
#include "session/player_world.h"

namespace ws {
constexpr int64_t kProfession = 108752746862200ll;

// the items of the cases
constexpr int64_t kBrooksteelAxe = 6064633866903617615ll;      // Gear_Weapon_Axe_1H_Brooksteel (item)
constexpr int64_t kBrooksteelAxeRecipe = 6064634092895538328ll;
constexpr int64_t kCopperFragments = 6064630417661755489ll;    // Resource_Processed_MetalFragments_Copper
constexpr int64_t kAshTimber = 6064629252204724244ll;          // Resource_Processed_Timber_Ash
constexpr int64_t kWeaponAxe = 6064634088230420900ll;          // Resource_Reagent_Weapon_Axe
constexpr int64_t kGolgoraRecipe = 6064633764891354166ll;      // Weaponsmithing_T2_Tool_Pickaxe_Golgora
constexpr int64_t kGolgora = 6064633764670301235ll;            // its product, worn in slot 26
constexpr int64_t kOakBoard = 6064630417661624409ll;
constexpr int64_t kBrassIngot = 6064630417661755493ll;
constexpr int64_t kMoldBasalt = 6064630417661165616ll;
constexpr int64_t kPickAxeReagent = 6064634673790713877ll;     // Resource_Reagent_Artisan_PickAxe
constexpr int64_t kTimberPick = 6064629252204724244ll;
constexpr int64_t kBoardPick = 6064630417661624409ll;
constexpr int64_t kDriedBoardPick = 6064631793829478457ll;
constexpr int64_t kMoldPick = 6064630417661165616ll;
constexpr int64_t kSlagReagentPick = 6064634081473029344ll;    // Resource_Byproduct_Slag (Item.Resource.Raw.Reagent)

// the stations
constexpr int64_t kSmithT1 = 6064630515162480654ll, kSmithT2 = 6064630519397089280ll, kSmithT3 = 6064630553810173952ll;
constexpr int64_t kSmithT4 = 6064630553810239489ll, kSmithT5 = 6064630553810305026ll, kCarpentryT2 = 6064630553904087057ll;
constexpr int64_t kGatherMiningT4 = 6064630656984743947ll, kGatherMiningT5 = 6064630656985595916ll;
constexpr int64_t kGatherHerbalismT4 = 6064630656990642192ll, kGatherMiningFTUE = 6064632377691668489ll;

// the three Apprentice Hunting Bows (Carpentry_T2_Tool_Bow_0..2)
constexpr int64_t kBowRecipe[3] = {6064632236840976474ll, 6064632236840976476ll, 6064632236840976478ll};
}  // namespace ws

/// The data with the gather-item filter table (contentc 47) read: the 30 filters that list no item then serve their input by the input's tags.  A
/// filter that lists items (the 21 of the design data) keeps its list where one was loaded.
static content::CraftRecipeData ws_with_filters(const content::CraftRecipeData& in) {
    content::CraftRecipeData out = in;
    out.filters_read = true;
    return out;
}

/// One piece of a bag for the cases: a Backpack stack of `record` at `quality`.
static svc::InventoryItem ws_item(int slot, int64_t record, int64_t quantity, int quality = 1) {
    svc::InventoryItem it = farm_item(slot, record, static_cast<int>(quantity));
    it.quality = quality;
    return it;
}

/// The item a filter or tag input is paid with in these tests: one of the filter's items when the table lists them (the player may bring any of
/// them), else one that carries the input's tags.
static int64_t ws_pick_for(const content::CraftRecipeData& data, const content::CraftInput& in) {
    namespace cr = aoc::worldd::crafting;
    const std::vector<std::string> tags = cr::tag_key(cr::split_tags(in.tags));
    if (in.filter != 0) {
        const auto it = data.filter_items.find(in.filter);
        if (it != data.filter_items.end() && !it->second.empty()) {
            for (const int64_t item : it->second) {
                bool all = true;
                for (const std::string& tag : tags) all = all && data.tag_admits(tag, item);
                if (all) return item;
            }
            return it->second.front();
        }
    }
    for (const std::string& key : tags) {
        if (key == "item.resource.processed.timber") return ws::kTimberPick;
        if (key == "item.resource.processed.board") return ws::kBoardPick;
        if (key == "item.resource.processed.driedboard") return ws::kDriedBoardPick;
        if (key == "item.resource.processed.metalfragment") return ws::kCopperFragments;
        if (key == "item.resource.processed.weaponmold") return ws::kMoldPick;
        if (key == "item.resource.raw.reagent") return ws::kSlagReagentPick;
        if (key == "item.resource.raw.plant") return farm::kDaffodil;
        if (key == "item.resource.raw.wood") return farm::kOak;
    }
    return 0;
}

struct WsStart {
    repl::StartCraft                req;
    std::vector<svc::InventoryItem> items;
};

/// The bag and the request that start `recipe`: the reagent, each general input named at its stack's rarity, and a pick for
/// each input named by a filter (one of the filter's items when the table lists them), in a tag selection.
static WsStart ws_start_for(const content::CraftRecipeData& data, const content::CraftRecipe& recipe, int quantity, const PlayerWorld& world, int quality = 1) {
    namespace cr = aoc::worldd::crafting;
    WsStart s;
    s.req = start_of(recipe, quantity);
    int slot = 0;
    for (const content::CraftInput& in : recipe.inputs) {
        if (in.quantity < 1) continue;
        const int need = in.quantity * quantity;
        if (in.primary) {
            const int q = std::max(quality, quality_at_least(world, in.item, in.min_rarity));
            s.items.push_back(ws_item(slot++, in.item, need, q));
            continue;
        }
        if (in.item != 0) {
            const int q = std::max(quality, quality_at_least(world, in.item, in.min_rarity));
            s.items.push_back(ws_item(slot++, in.item, need, q));
            s.req.general.push_back(resource(in.item, need, world.item_rarity_facts(in.item).rarity_of(q)));
            continue;
        }
        const std::vector<std::string> tags = cr::split_tags(in.tags);
        const int64_t pick = ws_pick_for(data, in);
        if (pick == 0) continue;   // (the recipe cannot be paid from these tests' bag)
        const int q = std::max(quality, quality_at_least(world, pick, in.min_rarity));
        s.items.push_back(ws_item(slot++, pick, need, q));
        s.req.tags.push_back(pick_of({resource(pick, need, world.item_rarity_facts(pick).rarity_of(q))}, tags));
    }
    return s;
}

/// The facts of a start at `station` in a Riverlands node of `level`, for a player of certification tier `tier`; `taught`
/// says every learnable recipe is known.
static crafting::StartFacts ws_facts(const content::CraftRecipeData& data, const content::CraftStationDef& station, const items::Inventory& bag, const PlayerWorld& world,
                                     int tier, bool taught, int level = 3) {
    crafting::StartFacts f;
    f.recipes = &data;
    f.station = &station;
    f.quick = true;   // (a crafting station's quick make, not a processing job)
    f.tier_of = [tier](int64_t) { return tier; };
    f.taught = [taught](int64_t) { return taught; };
    f.inventory = &bag;
    f.rarity_facts = [&world](int64_t item) { return world.item_rarity_facts(item); };
    f.tax = crafting::station_tax(station.tax_tag);
    f.copper = 5'000'000;
    f.node.level = level;
    f.node.biome = "Riverlands";
    return f;
}

/// The product facts a quick make reads: its tags (the slot factor and the experience) and its quality bands.
static std::optional<content::ItemSaleFacts> ws_sale(const content::ContentDb& db, int64_t item) {
    std::string error;
    auto f = db.item_sale(item, error);
    if (!f || !f->known) return std::nullopt;
    return f;
}

/// What the quick make pays with, from the plan's takes: the reagent does not count, the general inputs' stacks and rarities do.
static std::vector<crafting::PaidUnits> ws_paid(const crafting::Plan& plan) {
    std::vector<crafting::PaidUnits> paid;
    for (const crafting::Take& t : plan.takes) {
        if (t.lowest_first) continue;
        paid.push_back({t.slot, 1, t.count, items::default_rarity_facts().rarity_of(1)});
    }
    return paid;
}

/// The first product a recipe's reward table makes.
static std::optional<crafting::Product> ws_product(const content::CraftRecipeData& data, const content::CraftRecipe& recipe) {
    const content::CraftReward* reward = data.reward_of(recipe);
    if (!reward || reward->containers.empty()) return std::nullopt;
    const std::vector<crafting::Product> products = crafting::roll_products(*reward, 1, 0);
    return products.empty() ? std::nullopt : std::optional<crafting::Product>(products.front());
}

/// The data of the milestone: 763 recipes, the 34 tools, the 54 of every profession, their slots and the axe's stats.
static void test_ws_data(const content::ContentDb& db, const PlayerWorld& world) {
    (void)world;
    std::printf("Weaponsmithing's 763 recipes and the 54 Equipment-destination tools\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    if (!loaded) {
        std::printf("  [FAIL] the recipes do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    const content::CraftRecipeData& data = *loaded;
    size_t recipes = 0, equipment = 0, learnable = 0, tier2 = 0, master = 0, no_station = 0, build = 0, biome = 0, node_level = 0, filter = 0, no_product = 0, black = 0;
    const content::CraftStationData* stations = nullptr;
    std::string station_error;
    const auto station_data = db.craft_station_data(station_error);
    if (station_data) stations = &*station_data;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != ws::kProfession) continue;
        ++recipes;
        equipment += r.inventory_destination ? 0 : 1;
        learnable += r.learnable ? 1 : 0;
        tier2 += r.tier == 2 ? 1 : 0;
        master += r.tier >= 3 ? 1 : 0;
        black += r.blacklisted ? 1 : 0;
        bool has_build = false, has_biome = false, has_node = false, has_filter = false;
        for (const content::CraftPred& p : r.preds) {
            has_build = has_build || p.type == "BuildingModification";
            has_biome = has_biome || p.type == "Biome";
            has_node = has_node || p.type == "NodeLevel";
        }
        for (const content::CraftInput& in : r.inputs) has_filter = has_filter || (in.item == 0 && in.filter != 0);
        build += has_build ? 1 : 0;
        biome += has_biome ? 1 : 0;
        node_level += has_node ? 1 : 0;
        filter += has_filter ? 1 : 0;
        const content::CraftReward* reward = data.reward_of(r);
        no_product += (reward && !reward->containers.empty()) ? 0 : 1;
        bool any = false;
        if (stations) {
            for (const auto& entry : stations->defs) {
                if (entry.second.name.rfind("CraftingStation_Weaponsmithing_", 0) == 0 && data.set_holds(entry.second.recipe_set, guid)) any = true;
            }
        }
        no_station += any ? 0 : 1;
    }
    CHECK_SHOWN(recipes == 763, "Weaponsmithing holds 763 recipes");
    CHECK_SHOWN(equipment == 34, "34 of them deliver their product to the equipment storage");
    CHECK_SHOWN(learnable == 537, "537 have to be learned");
    CHECK_SHOWN(build == 513, "513 carry a BuildingModification predicate (the Smithy's service building)");
    CHECK_SHOWN(biome == 210 && node_level == 81, "210 carry a Biome predicate and 81 a NodeLevel one");
    CHECK_SHOWN(tier2 == 372 && master == 5, "372 are Journeyman recipes and 5 are Master or Grand Master");
    CHECK_SHOWN(filter == 30, "30 take an input named by a gather-item filter");
    CHECK_SHOWN(no_product == 3, "3 name no product the server can give");
    CHECK_SHOWN(black == 0 && no_station == 19, "none is switched off and 19 stand in no Weaponsmithing station set");

    // the 54 Equipment-destination recipes of the whole table, and the slot each product is worn in
    std::map<int, size_t> by_slot;
    size_t total_equipment = 0, not_tool = 0;
    for (const auto& [guid, r] : data.recipes) {
        if (r.inventory_destination) continue;
        ++total_equipment;
        const auto made = ws_product(data, r);
        if (!made) {
            ++not_tool;
            continue;
        }
        std::string error_item;
        const auto rec = db.item(made->item, error_item);
        if (!rec || rec->equip_slots.size() != 1) {
            ++not_tool;
            continue;
        }
        by_slot[rec->equip_slots.front()]++;
    }
    CHECK_SHOWN(total_equipment == 54, "54 recipes of the whole table deliver to the equipment storage");
    CHECK_SHOWN(by_slot.size() == 5 && by_slot[26] == 15 && by_slot[29] == 10 && by_slot[33] == 11 && by_slot[35] == 9 && by_slot[38] == 9,
                "the 54 products are worn in one tool slot each: Pickaxe 15, Axe 10, Sickle 11, Fishing Rod 9, Bow 9");
    CHECK_SHOWN(not_tool == 0, "every one of the 54 products is a tool of exactly one slot");

    // the Brooksteel Axe's own data: its rarity bounds, its quality bands and the durability at each rarity
    std::string item_error;
    const auto axe = db.item(ws::kBrooksteelAxe, item_error);
    CHECK_SHOWN(axe.has_value(), "the Brooksteel Axe is in the item table");
    if (!axe) return;
    CHECK_SHOWN(axe->default_quality == 1 && axe->equip_slots == std::vector<uint8_t>{items::equip::Primary} && axe->max_stack_size == 1,
                "the axe starts at quality 1, is one in a stack and is worn in the primary hand");
    CHECK_SHOWN(axe->rarity_min == 0 && axe->rarity_max == 5 && axe->has_curve, "its rarity runs Common to Legendary on its own quality curve");
    const double bands[] = {1, 6, 16, 31, 51, 71};
    bool bands_ok = true;
    for (int i = 0; i < 6; ++i) bands_ok = bands_ok && std::fabs(axe->bands[static_cast<size_t>(i)].x - bands[i]) < 1e-9;
    CHECK_SHOWN(bands_ok, "its quality bands start at 1, 6, 16, 31, 51 and 71: Common to Legendary");
    const double durability[] = {13, 15, 16, 18, 19, 22};
    bool dur_ok = true;
    for (int i = 0; i < 6; ++i) dur_ok = dur_ok && std::fabs(axe->max_durability_at(static_cast<int>(bands[i])) - durability[i]) < 0.01;
    CHECK_SHOWN(dur_ok, "its maximum durability at those bands is 13, 15, 16, 18, 19 and 22 (the MaxDurability stat block)");
    CHECK_SHOWN(axe->rarity_of(1) == 0 && axe->rarity_of(10) == 1 && axe->rarity_of(52) == 4, "quality 1 reads Common, 10 Uncommon, 52 Epic");

    // the stations: the profession's tiers 0 to 4, and the tools' family
    CHECK_SHOWN(stations != nullptr, "the station definitions read");
    if (!stations) return;
    const content::CraftStationDef* t1 = station_named(*stations, "CraftingStation_Weaponsmithing_T1");
    const content::CraftStationDef* t2 = station_named(*stations, "CraftingStation_Weaponsmithing_T2");
    const content::CraftStationDef* t3 = station_named(*stations, "CraftingStation_Weaponsmithing_T3");
    const content::CraftStationDef* t4 = station_named(*stations, "CraftingStation_Weaponsmithing_T4");
    const content::CraftStationDef* t5 = station_named(*stations, "CraftingStation_Weaponsmithing_T5");
    CHECK_SHOWN(t1 && t1->max_tier == 0 && t2 && t2->max_tier == 1 && t3 && t3->max_tier == 2 && t4 && t4->max_tier == 3 && t5 && t5->max_tier == 4,
                "the Weaponsmithing stations reach Novice 0, Apprentice 1, Journeyman 2, Master 3 and Grand Master 4");
    size_t gathering = 0;
    for (const auto& entry : stations->defs) gathering += entry.second.name.rfind("CraftingStation_GatheringTools_", 0) == 0 ? 1 : 0;
    CHECK_SHOWN(gathering == 26, "the 26 stations of gathering tools (Fishing, Herbalism, Hunting, Lumberjacking and Mining, five tiers each and the Mining FTUE)");
    SAYOK("Weaponsmithing's data and the tools");
}

/// What starts at each station, per player tier, with and without the gather-item filter table (contentc 47).
static void test_ws_plan(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("what starts at the Weaponsmithing stations, a fresh character and one taught every recipe\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    if (!loaded) {
        std::printf("  [FAIL] the recipes do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    const content::CraftRecipeData served = ws_with_filters(*loaded);
    std::string station_error;
    const auto station_data = db.craft_station_data(station_error);
    CHECK_SHOWN(station_data.has_value(), "the station definitions read");
    if (!station_data) return;
    const content::CraftStationDef* t1 = station_named(*station_data, "CraftingStation_Weaponsmithing_T1");
    const content::CraftStationDef* t2 = station_named(*station_data, "CraftingStation_Weaponsmithing_T2");
    const content::CraftStationDef* t3 = station_named(*station_data, "CraftingStation_Weaponsmithing_T3");
    CHECK_SHOWN(t1 && t2 && t3, "the three station definitions of the cases are in the data");
    if (!t1 || !t2 || !t3) return;

    // the station's own recipes that start, for a bag built from the recipe's inputs
    const auto starts = [&](const content::CraftRecipeData& data, const content::CraftStationDef& station, int tier, bool taught, int level) {
        int ok = 0;
        std::map<std::string, int> refused;
        for (const auto& [guid, r] : data.recipes) {
            if (r.profession != ws::kProfession || !data.set_holds(station.recipe_set, guid)) continue;
            const WsStart start = ws_start_for(data, r, 1, world);
            auto bag = bag_of(start.items);
            if (!bag) {
                ++refused["the bag does not read"];
                continue;
            }
            const cr::Plan plan = cr::plan_start(start.req, ws_facts(data, station, *bag, world, tier, taught, level));
            if (plan.ok) {
                ++ok;
            } else if (plan.refusal.find("has to be learned") != std::string::npos) {
                ++refused["learnable"];
            } else if (plan.refusal.find("needs certification tier") != std::string::npos) {
                ++refused["cert"];
            } else if (plan.refusal.find("made only in the biomes") != std::string::npos) {
                ++refused["biome"];
            } else if (plan.refusal.find("needs a service building") != std::string::npos) {
                ++refused["building"];
            } else if (plan.refusal.find("lists no items for filters") != std::string::npos) {
                ++refused["filter"];
            } else if (plan.refusal.find("reaches") != std::string::npos && plan.refusal.find("needs tier") != std::string::npos) {
                ++refused["station tier"];
            } else {
                ++refused["other"];
            }
        }
        std::printf("    %s at tier %d, %s: %d start", station.name.c_str(), tier, taught ? "taught" : "fresh", ok);
        for (const auto& [why, n] : refused) std::printf("; %s %d", why.c_str(), n);
        std::printf("\n");
        return std::make_pair(ok, refused);
    };

    // a file without the gather-item filter table (contentc before 47), built from this file's recipes: the filter inputs wait on the table, and the
    // counts are the ones written before the table existed (an older content.db is the same data with no craft_filter_item)
    {
        content::CraftRecipeData older = *loaded;
        older.filter_items.clear();
        older.filters_read = false;
        CHECK_SHOWN(!older.filters_read && older.filter_items.empty(), "the older file has no gather-item filter table (contentc before 47)");
        const auto fresh1 = starts(older, *t1, 0, false, 0);
        CHECK_SHOWN(fresh1.first == 20, "a fresh Novice at Novice Weaponsmithing starts 20 of its 124 recipes (the 19 Brooksteel weapons and the ScalpHook)");
        CHECK_SHOWN(fresh1.second.count("filter") && fresh1.second.at("filter") == 3, "the 3 Novice tools wait on the gather-item filter the file has no table for");
        const auto taught1 = starts(older, *t1, 0, true, 0);
        CHECK_SHOWN(taught1.first == 98, "taught every recipe, the same Novice starts 98: the 2 FTUE weapons and the 3 tools wait on the filter");
        const auto fresh2 = starts(older, *t2, 1, false, 3);
        CHECK_SHOWN(fresh2.first == 39, "a fresh Apprentice at Apprentice Weaponsmithing starts 39 of its 380");
        const auto taught2 = starts(older, *t2, 1, true, 3);
        CHECK_SHOWN(taught2.first == 122, "taught every recipe, the Apprentice starts 122: the 13 tools and the 2 FTUE weapons wait on the filter");
    }
    // the gather-item filter table (contentc 47): a filter that lists items takes one of them, and the 30 that list none serve their input by its tags
    {
        const auto fresh1 = starts(served, *t1, 0, false, 0);
        CHECK_SHOWN(fresh1.first == 23, "with the filter table, a fresh Novice at Novice Weaponsmithing starts 23: the 20 and its 3 tools");
        CHECK_SHOWN(fresh1.second.count("biome") && fresh1.second.at("biome") == 19, "the other 19 of the set stand in TabletopMountains and are refused");
        const auto taught1 = starts(served, *t1, 0, true, 0);
        CHECK_SHOWN(taught1.first == 103, "taught every recipe, the Novice starts 103 of its 124: 19 off-biome and 2 needing Apprentice");
        CHECK_SHOWN(!taught1.second.count("filter") && !taught1.second.count("other"), "no taught recipe of the Novice station waits on the gather-item filter: the 3 tools and the 2 FTUE weapons take items its lists hold");
        const auto fresh2 = starts(served, *t2, 0, false, 3);
        CHECK_SHOWN(fresh2.first == 23, "a fresh Novice at the Apprentice station starts 23 (the 3 Novice tools, the Apprentice ones need the tier)");
        const auto fresh2b = starts(served, *t2, 1, false, 3);
        CHECK_SHOWN(fresh2b.first == 45, "a fresh Apprentice starts 45: the 39 and the 3 Novice and 3 Apprentice tools that need no learning");
        const auto taught2 = starts(served, *t2, 1, true, 3);
        CHECK_SHOWN(taught2.first == 136 && taught2.second.count("building") && taught2.second.at("building") == 168,
                    "taught every recipe, the Apprentice starts 136 of 380; 168 wait on the Smithy's service building");
        CHECK_SHOWN(!taught2.second.count("filter") && !taught2.second.count("other"), "no taught recipe of the Apprentice station waits on the filter: its 14 filter inputs are items of its lists");
        for (int tier = 2; tier <= 4; ++tier) {
            const auto taught3 = starts(served, *t3, tier, true, 3);
            CHECK_SHOWN(taught3.first == 152, tier == 2 ? "taught, a Journeyman at a placed Journeyman station starts 152 of 744: the 29 tools it lists among them"
                                                        : "... and every higher certification starts the same 152: the station reaches Journeyman");
            CHECK_SHOWN(!taught3.second.count("filter") && !taught3.second.count("other"), tier == 2 ? "no taught recipe of the Journeyman station waits on the filter: its 30 filter inputs are items of its lists"
                                                                                                     : "... and the same holds at the higher certifications");
        }
        const auto fresh3 = starts(served, *t3, 2, false, 3);
        CHECK_SHOWN(fresh3.first == 48, "a fresh Journeyman starts 48 at a placed Journeyman station (the 45 and the 3 Journeyman tools that need no learning)");
    }
    // the 2 FTUE weapons' 8 filter inputs: the bag holds one of each filter's items (the raw-reagent filter lists 2 Biome reagents)
    {
        size_t filter_inputs = 0, admitted = 0;
        int64_t reagent_filter = 0;
        for (const char* name : {"Weaponsmithing_T1_Weapon_Scepter_1H_FTUE", "Weaponsmithing_T1_Weapon_Sword_2H_FTUE"}) {
            const content::CraftRecipe* weapon = recipe_named(served, name);
            if (!weapon) continue;
            for (const content::CraftInput& in : weapon->inputs) {
                if (in.filter == 0) continue;
                ++filter_inputs;
                if (in.tags.find("Raw.Reagent") != std::string::npos) reagent_filter = in.filter;
                admitted += served.filter_items.count(in.filter) == 0 || served.filter_admits(in.filter, ws_pick_for(served, in)) ? 1 : 0;
            }
        }
        const auto listed = served.filter_items.find(reagent_filter);
        CHECK_SHOWN(filter_inputs == 8 && admitted == 8, "the 2 FTUE weapons' 8 filter inputs take an item their filter lists");
        CHECK_SHOWN(reagent_filter != 0 && listed != served.filter_items.end() && listed->second.size() == 2,
                    "their raw-reagent filter lists 2 Biome reagents, and the bag's pick is one of them");
    }

    // the Novice plan's price, time and takes: 50 copper, the default half plus one, 2 s, the reagent first
    const content::CraftRecipe* axe_recipe = recipe_named(served, "Weaponsmithing_T1_Weapon_Axe_1H_Brooksteel");
    CHECK_SHOWN(axe_recipe != nullptr, "the Brooksteel Axe's recipe is in the data");
    if (!axe_recipe) return;
    const WsStart axe_start = ws_start_for(served, *axe_recipe, 1, world);
    auto axe_bag = bag_of(axe_start.items);
    CHECK_SHOWN(axe_bag.has_value(), "a bag of 5 Copper MetalFragments, an Ash Timber and the axe reagent");
    if (!axe_bag) return;
    const cr::Plan axe_plan = cr::plan_start(axe_start.req, ws_facts(served, *t1, *axe_bag, world, 0, false, 0));
    CHECK_SHOWN(axe_plan.ok, "a fresh Novice starts the Brooksteel Axe at Novice Weaponsmithing");
    if (!axe_plan.ok) {
        std::printf("    %s\n", axe_plan.refusal.c_str());
        return;
    }
    CHECK_SHOWN(axe_plan.unit_cost == 50 && axe_plan.unit_taxed == 75 && axe_plan.cost == 75, "it costs 50 copper, 75 with the Smithy service tax of half the price");
    CHECK_SHOWN(axe_plan.duration_ms == 2000, "it runs 2 s (the recipe's base duration)");
    CHECK_SHOWN(axe_plan.takes.size() == 3 && axe_plan.takes.front().lowest_first && axe_plan.takes.front().record == ws::kWeaponAxe, "the reagent is taken first");

    // the tool slot's own recipe: the Golgora pickaxe, its plan and its quality, durability and experience
    const content::CraftRecipe* golgora = recipe_named(served, "Weaponsmithing_T2_Tool_Pickaxe_Golgora");
    CHECK_SHOWN(golgora != nullptr && !golgora->inventory_destination && golgora->tier == 1 && golgora->learnable, "the Golgora pickaxe is a tier-1 tool that must be learned");
    if (!golgora) return;
    const WsStart golgora_start = ws_start_for(served, *golgora, 1, world);
    auto golgora_bag = bag_of(golgora_start.items);
    CHECK_SHOWN(golgora_bag.has_value(), "a bag of 10 Oak Boards, 10 Brass Ingots and a Basalt Weapon Mold");
    if (!golgora_bag) return;
    const cr::Plan not_taught = cr::plan_start(golgora_start.req, ws_facts(served, *t2, *golgora_bag, world, 1, false, 3));
    CHECK_SHOWN(!not_taught.ok && not_taught.refusal.find("has to be learned") != std::string::npos, "not taught, it is refused for the learning");
    const cr::Plan golgora_plan = cr::plan_start(golgora_start.req, ws_facts(served, *t2, *golgora_bag, world, 1, true, 3));
    CHECK_SHOWN(golgora_plan.ok, "an Apprentice who was taught it starts the pickaxe at Apprentice Weaponsmithing");
    if (!golgora_plan.ok) {
        std::printf("    %s\n", golgora_plan.refusal.c_str());
        return;
    }
    const std::vector<cr::PaidUnits> paid = ws_paid(golgora_plan);
    const auto made = cr::roll_craft(served, *golgora_plan.recipe, paid, golgora_plan.ratings, 1, 12345, [&](int64_t item) { return ws_sale(db, item); });
    CHECK_SHOWN(made.has_value() && made->products.size() == 1 && made->products.front().item == ws::kGolgora, "the quick make makes Gear_Artisan_Mining_Tool_Pickaxe_T2_Golgora");
    if (!made) return;
    std::printf("    (the Golgora roll: quality %d, rarity %d, xp %lld; the sale facts %s)\n", made->quality, made->rarity, static_cast<long long>(made->profession_xp),
                ws_sale(db, ws::kGolgora).has_value() ? "known" : "none");
    CHECK_SHOWN(made->quality == 1 && made->rarity == 2, "all-Common materials make it quality 1; its record is fixed at Rare, so it reads Rare");
    CHECK_SHOWN(made->profession_xp == 1200, "it pays 1,200 Weaponsmithing experience (Apprentice 200 x Rare's 3 x the tool's 2.0 slot factor)");
    CHECK_SHOWN(made->adventuring_xp == 0, "and no Adventuring experience: the tool family names the gathering rule the reader maps to none");

    // the axe: 240 XP, Common quality 1, durability 13; all-Uncommon materials come out Uncommon, 6, durability 15, 480 XP
    const std::vector<cr::PaidUnits> axe_paid = ws_paid(axe_plan);
    const auto axe_made = cr::roll_craft(served, *axe_plan.recipe, axe_paid, axe_plan.ratings, 1, 1, [&](int64_t item) { return ws_sale(db, item); });
    CHECK_SHOWN(axe_made.has_value() && axe_made->quality == 1 && axe_made->rarity == 0 && axe_made->profession_xp == 240, "the axe pays 240 experience at Common quality 1");
    std::vector<cr::PaidUnits> uncommon;
    uncommon.push_back({1, 6, 5, 1});   // the fragments' slot
    uncommon.push_back({2, 6, 1, 1});   // the timber's slot
    const auto better = cr::roll_craft(served, *axe_plan.recipe, uncommon, axe_plan.ratings, 1, 1, [&](int64_t item) { return ws_sale(db, item); });
    CHECK_SHOWN(better.has_value() && better->quality == 6 && better->rarity == 1 && better->profession_xp == 480,
                "all-Uncommon materials make it quality 6, Uncommon, and pay 480 (the band it starts in is the first of Uncommon)");

    // the items as crafted: durability from the item record at the quality
    std::string axe_error;
    const auto axe_item = db.item(ws::kBrooksteelAxe, axe_error);
    CHECK_SHOWN(axe_item && std::fabs(axe_item->max_durability_at(1) - 13.0) < 0.01 && std::fabs(axe_item->max_durability_at(6) - 15.0) < 0.01,
                "the axe's durability at the two qualities is 13 and 15");
    std::string error_item;
    const auto golgora_item = db.item(ws::kGolgora, error_item);
    std::printf("    (the Golgora item: %s, durability at 1: %.1f, a durability stat: %s)\n", golgora_item ? "known" : "none",
                golgora_item ? golgora_item->max_durability_at(1) : -1.0, golgora_item && golgora_item->durability_stat ? "yes" : "no");
    CHECK_SHOWN(golgora_item && std::fabs(golgora_item->max_durability_at(1) - 300.0) < 0.01, "the Golgora pickaxe's durability at its Rare quality 1 is 300");
    SAYOK("the station counts, the price and the crafted numbers");
}

/// The Equipment destination's delivery: the tool slot when it is free, the bag otherwise; all 54 tools start at a station.
static void test_ws_tools(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("the 54 gathering tools: every recipe that has a station starts there, and the product goes to its tool slot\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    if (!loaded) {
        std::printf("  [FAIL] the recipes do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    const content::CraftRecipeData data = ws_with_filters(*loaded);
    std::string station_error;
    const auto station_data = db.craft_station_data(station_error);
    CHECK_SHOWN(station_data.has_value(), "the station definitions read");
    if (!station_data) return;

    // every one of the 54: a station that lists it, its tier at that station, and a taught player
    int startable = 0, no_site = 0;
    std::vector<std::string> homeless;
    for (const auto& [guid, r] : data.recipes) {
        if (r.inventory_destination) continue;
        const content::CraftStationDef* site = nullptr;
        for (const auto& entry : station_data->defs) {
            const content::CraftStationDef& st = entry.second;
            if (!data.set_holds(st.recipe_set, guid)) continue;
            if (st.max_tier >= 0 && r.tier > st.max_tier) continue;
            // prefer the profession's own station over the tools' family
            const bool own = st.name.rfind("CraftingStation_Weaponsmithing_", 0) == 0 || st.name.rfind("CraftingStation_Carpentry_", 0) == 0;
            if (!site || (own && site->name.rfind("CraftingStation_GatheringTools_", 0) == 0)) site = &st;
        }
        if (!site) {
            ++no_site;
            homeless.push_back(r.name);
            continue;
        }
        const WsStart start = ws_start_for(data, r, 1, world);
        auto bag = bag_of(start.items);
        if (!bag) continue;
        const int level = site->name.find("_T1") != std::string::npos || site->name.find("FTUE") != std::string::npos ? 0 : 3;
        const cr::Plan plan = cr::plan_start(start.req, ws_facts(data, *site, *bag, world, std::max(r.tier, 0), true, level));
        if (plan.ok) ++startable;
    }
    CHECK_SHOWN(startable == 52, "52 of the 54 tool recipes start with their inputs, a taught crafter and a station that lists them");
    CHECK_SHOWN(no_site == 1 && homeless.size() == 1 && homeless.front().rfind("OBSOLETE_", 0) == 0, "1 stands in no station set: the retired Lumberjacking starter tool");
    const content::CraftRecipe* befallen = recipe_named(data, "Weaponsmithing_T3_Tool_Pickaxe_Befallen");
    bool dungeon = false;
    if (befallen) {
        for (const auto& entry : station_data->defs) {
            if (entry.second.name == "CraftingStation_BefallenForge" && data.set_holds(entry.second.recipe_set, befallen->guid)) dungeon = true;
        }
    }
    CHECK_SHOWN(befallen && dungeon, "the Befallen pickaxe's only station is the Befallen Forge, which the quick make does not serve");

    // Weaponsmithing's 34: 33 start at a Weaponsmithing or gathering-tools station; the Befallen one at none
    int smith_tools = 0, smith_start = 0;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != ws::kProfession || r.inventory_destination) continue;
        ++smith_tools;
        const content::CraftStationDef* site = nullptr;
        for (const auto& entry : station_data->defs) {
            const content::CraftStationDef& st = entry.second;
            if (!data.set_holds(st.recipe_set, guid)) continue;
            if (st.max_tier >= 0 && r.tier > st.max_tier) continue;
            const bool own = st.name.rfind("CraftingStation_Weaponsmithing_", 0) == 0;
            if (!site || (own && site->name.rfind("CraftingStation_GatheringTools_", 0) == 0)) site = &st;
        }
        if (!site) continue;
        const WsStart start = ws_start_for(data, r, 1, world);
        auto bag = bag_of(start.items);
        if (!bag) continue;
        const int level = site->name.find("_T1") != std::string::npos ? 0 : 3;
        const cr::Plan plan = cr::plan_start(start.req, ws_facts(data, *site, *bag, world, std::max(r.tier, 0), true, level));
        if (plan.ok) ++smith_start;
    }
    CHECK_SHOWN(smith_tools == 34 && smith_start == 33, "33 of Weaponsmithing's 34 tools start at a station that lists them (the Master and Grand Master ones at the tools' own family)");

    // the three Apprentice Hunting Bows the five Apprentice hunts need: at Carpentry - Apprentice, with a tier-1 crafter
    const content::CraftStationDef* carpentry = station_named(*station_data, "CraftingStation_Carpentry_T2");
    CHECK_SHOWN(carpentry != nullptr && carpentry->max_tier == 1, "Carpentry - Apprentice is in the data at tier 1");
    if (!carpentry) return;
    int bows = 0;
    for (const int64_t guid : ws::kBowRecipe) {
        const content::CraftRecipe* bow = data.recipe(guid);
        if (!bow) continue;
        const WsStart start = ws_start_for(data, *bow, 1, world);
        auto bag = bag_of(start.items);
        if (!bag) continue;
        const cr::Plan plan = cr::plan_start(start.req, ws_facts(data, *carpentry, *bag, world, 1, true, 3));
        if (plan.ok && !bow->inventory_destination) ++bows;
    }
    CHECK_SHOWN(bows == 3, "Carpentry_T2_Tool_Bow_0, _1 and _2 start at Carpentry - Apprentice (learned where they must be)");

    // the old refusal is gone: no Equipment recipe is refused for its destination
    const content::CraftRecipe* pickaxe = recipe_named(data, "Weaponsmithing_T1_Tool_Pickaxe_0");
    CHECK_SHOWN(pickaxe != nullptr && !pickaxe->inventory_destination, "the Novice Pickaxe's recipe is an Equipment one");
    if (pickaxe) {
        const content::CraftStationDef* t1 = station_named(*station_data, "CraftingStation_Weaponsmithing_T1");
        const WsStart start = ws_start_for(data, *pickaxe, 1, world);
        auto bag = bag_of(start.items);
        const cr::Plan plan = bag ? cr::plan_start(start.req, ws_facts(data, *t1, *bag, world, 0, false, 0)) : cr::Plan{};
        CHECK_SHOWN(plan.ok, "a Novice starts the pickaxe at Novice Weaponsmithing: it is no longer refused as worn at once");
        CHECK_SHOWN(!plan.ok || plan.refusal.find("worn at once") == std::string::npos, "the refusal text \"worn at once\" is gone");
    }
    SAYOK("the 54 tools and the bows");
}

/// The tool slot's grant: into the Equipment when its slot is free, refused when it is taken, and no bag room needed.
static void test_ws_grant(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("a tool's delivery: the tool slot when it is free, refused when it is taken\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    std::string station_error;
    const auto station_data = db.craft_station_data(station_error);
    const content::CraftRecipeData* data = loaded ? &*loaded : nullptr;
    const content::CraftStationDef* t2 = station_data ? station_named(*station_data, "CraftingStation_Weaponsmithing_T2") : nullptr;
    const content::CraftRecipe* golgora = data ? recipe_named(*data, "Weaponsmithing_T2_Tool_Pickaxe_Golgora") : nullptr;
    CHECK_SHOWN(data && t2 && golgora, "the Golgora recipe and its station are in the data");
    if (data && t2 && golgora) {
        content::CraftRecipeData served = ws_with_filters(*data);
        const WsStart start = ws_start_for(served, *golgora, 1, world);
        auto bag = bag_of(start.items);
        const cr::Plan plan = bag ? cr::plan_start(start.req, ws_facts(served, *t2, *bag, world, 1, true, 3)) : cr::Plan{};
        CHECK_SHOWN(plan.ok, "an Apprentice who was taught the tool makes it: the Equipment destination is served");
    }
    items::ContentItemCatalog catalog(db);
    auto empty = bag_of({});
    CHECK_SHOWN(empty.has_value(), "an empty bag");
    if (!empty) return;
    svc::InventoryItem pickaxe = ws_item(0, ws::kGolgora, 1);
    pickaxe.container = svc::ItemContainer::Backpack;
    pickaxe.slot = 0;
    items::Outcome worn = empty->grant_equipment(pickaxe, items::equip::Pickaxe, catalog);
    CHECK_SHOWN(worn.ok && worn.equipment_changed && worn.events.size() == 1, "the tool goes to the equipment: one Add");
    const svc::InventoryItem* there = empty->at(svc::ItemContainer::Equipment, items::equip::Pickaxe);
    CHECK_SHOWN(there && there->item_record_id == ws::kGolgora && there->quantity == 1, "it lies in the Pickaxe slot 26");
    CHECK_SHOWN(worn.events.front().container == svc::ItemContainer::Equipment && worn.events.front().slot == items::equip::Pickaxe,
                "the Add names the Equipment and slot 26");
    svc::InventoryItem second = ws_item(0, ws::kBrooksteelAxe, 1);
    second.instance_guid = empty->fresh_guid();
    const items::Outcome taken = empty->grant_equipment(second, items::equip::Primary, catalog);   // the axe's slot is free, the pickaxe's is not
    CHECK_SHOWN(taken.ok, "a second tool in another free slot is taken");
    svc::InventoryItem third = ws_item(0, ws::kBrooksteelAxe, 1);
    third.instance_guid = empty->fresh_guid();
    const items::Outcome refused = empty->grant_equipment(third, items::equip::Primary, catalog);
    CHECK_SHOWN(!refused.ok && refused.refusal.find("holds") != std::string::npos, "a tool into a slot that holds one is refused, changing nothing");
    svc::InventoryItem wrong = ws_item(0, ws::kGolgora, 1);
    wrong.instance_guid = empty->fresh_guid();
    const items::Outcome wrong_slot = empty->grant_equipment(wrong, items::equip::Helmet, catalog);
    CHECK_SHOWN(!wrong_slot.ok && wrong_slot.refusal.find("not a slot") != std::string::npos, "a tool into a slot its record does not list is refused");
    const items::Outcome no_tool = empty->grant_equipment(ws_item(0, ws::kAshTimber, 1), items::equip::Pickaxe, catalog);
    CHECK_SHOWN(!no_tool.ok, "an item that is not worn is refused");
    SAYOK("the tool slot's grant");
}

/// The ServiceBuilding holdback: the Smithy's 513 recipes and the missing Journeyman station, with the numbers.
static void test_ws_smithy(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("the Smithy's service building and the missing Journeyman stations: what is held back\n");
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    if (!loaded) {
        std::printf("  [FAIL] the recipes do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    const content::CraftRecipeData data = ws_with_filters(*loaded);
    std::string station_error;
    const auto station_data = db.craft_station_data(station_error);
    if (!station_data) return;
    const content::CraftStationDef* t3 = station_named(*station_data, "CraftingStation_Weaponsmithing_T3");
    CHECK_SHOWN(t3 && t3->max_tier == 2, "the Journeyman station definition reaches tier 2");
    if (!t3) return;

    // a taught Journeyman recipe with a BuildingModification predicate passes the station and tier and stops at the building
    int holding = 0;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != ws::kProfession || r.tier != 2 || !data.set_holds(t3->recipe_set, guid)) continue;
        bool build = false;
        for (const content::CraftPred& p : r.preds) build = build || p.type == "BuildingModification";
        if (build) ++holding;
    }
    CHECK_SHOWN(holding == 339, "339 of the taught Journeyman recipes wait on the Smithy's service building");
    std::printf("    %d Journeyman recipes of the T3 set carry the building predicate\n", holding);
    const content::CraftRecipe* held = recipe_named(data, "Weaponsmithing_T3_Weapon_Axe_1H_Academant");
    CHECK_SHOWN(held != nullptr, "the Academant axe is one of them");
    if (held) {
        bool build = false;
        for (const content::CraftPred& p : held->preds) build = build || p.type == "BuildingModification";
        CHECK_SHOWN(build && held->tier == 2 && data.set_holds(t3->recipe_set, held->guid), "it is a Journeyman recipe of the station's set with the Smithy's predicate");
        const WsStart start = ws_start_for(data, *held, 1, world);
        auto bag = bag_of(start.items);
        const cr::Plan plan = bag ? cr::plan_start(start.req, ws_facts(data, *t3, *bag, world, 2, true, 3)) : cr::Plan{};
        CHECK_SHOWN(!plan.ok && plan.refusal.find("needs a service building in the node") != std::string::npos,
                    "it is refused for the service building, not for the station, the tier or the learning");
    }

    // no node pre-builds the Smithy: the 3 modification records name it; 0 service plots have it pre-constructed (the Python check)
    size_t modifications = 0;
    std::set<std::string> mods;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != ws::kProfession) continue;
        for (const content::CraftPred& p : r.preds) {
            if (p.type != "BuildingModification") continue;
            ++modifications;
            mods.insert(p.modifications);
        }
    }
    CHECK_SHOWN(modifications == 513 && mods.size() == 3, "the 513 recipes name the Smithy's three modification records (189, 185 and 139 recipes)");
    size_t in_t3 = 0;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != ws::kProfession || !data.set_holds(t3->recipe_set, guid)) continue;
        for (const content::CraftPred& p : r.preds) in_t3 += p.type == "BuildingModification" ? 1 : 0;
    }
    CHECK_SHOWN(in_t3 == 507, "507 of the 513 stand in the Journeyman station's set: the other 6 are in no station");
    const auto t4 = station_named(*station_data, "CraftingStation_Weaponsmithing_T4");
    const auto t5 = station_named(*station_data, "CraftingStation_Weaponsmithing_T5");
    CHECK_SHOWN(t4 && t4->max_tier == 3 && t5 && t5->max_tier == 4, "the Master and Grand Master station definitions are in the data (tiers 3 and 4)");
    SAYOK("the Smithy's holdback");
}

