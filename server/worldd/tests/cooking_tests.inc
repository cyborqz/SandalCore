// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  cooking_tests.inc: the Cooking station's Novice and Apprentice recipes (the Cooking milestone)
//
//  The real content.db: the Cooking - Apprentice station (ProcessingStation_Cooking_T2, max tier 1) holds 81 recipes, 22 Novice and 59 Apprentice; the
//  Novice one holds the 22.  Each recipe starts from the game's own items (the inputs it names, a pick of the fish tag for its filter input), runs for its
//  base time and pays its profession experience (50 a tier-0 unit, 60 a tier-1 unit at Common) and its product.  The checks that decide a start: the
//  station's tier, the player's certification, the recipe that has to be learned, the gather-item filter input, the bags and the purse.  The same station
//  with a second player, the same start twice and the saved job a reconnect reads are checked on the same recipes.
//  Included by test_worldd_professions.cpp after its helpers, before main().
// ============================================================================

namespace cook {
constexpr int64_t kCooking = 108752757020296ll;
constexpr int64_t kApprenticeCert = 108906340767353ll;
constexpr int64_t kBluegill = 6064629326176452611ll;
constexpr int64_t kDaffodil = 6064629184431849472ll;
constexpr int64_t kFriedBluegillRecipeItem = 6064632112030548224ll;   // Consumable_Recipe_Cooking_T2_FriedBluegill
constexpr int64_t kFriedBluegill = 6064632002346156226ll;             // Cooking_T2_Food_FriedBluegill
}  // namespace cook

/// The quality of an item at its lowest value that reaches `rarity` (a shop sells a reagent at quality 1).
static int quality_at_least(const PlayerWorld& world, int64_t item, int rarity) {
    const items::ItemRarityFacts facts = world.item_rarity_facts(item);
    for (int q = 1; q <= 200; ++q) {
        if (facts.rarity_of(q) >= rarity) return q;
    }
    return 1;
}

/// What one start names and holds: the bag and the request's general resources and tag picks, for `quantity` units.
struct CookStart {
    repl::StartCraft                req;
    std::vector<svc::InventoryItem> items;
};

/// An item that carries all of `tags` (lower case), the fish first for the bait's tag; 0 when the data has none.
static int64_t item_with_tags(const content::CraftRecipeData& data, const std::vector<std::string>& tags) {
    std::vector<int64_t> common;
    bool first = true;
    for (const std::string& raw : tags) {
        std::string tag;
        for (const char c : raw) tag.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        const auto it = data.tag_items.find(tag);
        const std::vector<int64_t> none;
        const std::vector<int64_t>& have = it == data.tag_items.end() ? none : it->second;
        if (first) {
            common = have;
            first = false;
        } else {
            std::vector<int64_t> both;
            std::set_intersection(common.begin(), common.end(), have.begin(), have.end(), std::back_inserter(both));
            common = std::move(both);
        }
    }
    if (tags.size() == 1 && tags.front() == "Item.Resource.Raw.Fish" && std::binary_search(common.begin(), common.end(), cook::kBluegill)) return cook::kBluegill;
    return common.empty() ? 0 : common.front();
}

/// The bag and the request that start `recipe` for `quantity` units, from the inputs the game names.  A filter input lists no item in the file this run has,
/// so its pick is an item of its tags (what a contentc-47 table leaves the pick to for the baits).  With `shop` the reagents are the quality a shop sells them at (the item's default
/// quality: what the Marketplace's goods are made at), not the lowest quality that meets the recipe.
static CookStart cooking_start(const content::CraftRecipeData& data, const content::CraftRecipe& recipe, const PlayerWorld& world, int quantity, const content::ContentDb* shop = nullptr) {
    CookStart s;
    s.req = start_of(recipe, quantity);
    int slot = 0;
    std::map<std::pair<int64_t, int>, int64_t> named;   // (record, rarity) -> count the request names
    for (const content::CraftInput& in : recipe.inputs) {
        if (in.quantity < 1) continue;
        const int64_t need = static_cast<int64_t>(in.quantity) * quantity;
        if (in.primary) {
            int q = quality_at_least(world, in.item, in.min_rarity);
            if (shop) {
                std::string error;
                const auto rec = shop->item(in.item, error);
                q = rec ? std::max(1, rec->default_quality) : 1;
            }
            s.items.push_back(farm_item(slot++, in.item, static_cast<int>(need)));
            s.items.back().quality = q;
            continue;
        }
        if (in.item != 0) {
            const int q = quality_at_least(world, in.item, in.min_rarity);
            s.items.push_back(farm_item(slot++, in.item, static_cast<int>(need)));
            s.items.back().quality = q;
            named[{in.item, world.item_rarity_facts(in.item).rarity_of(q)}] += need;
            continue;
        }
        int64_t pick = 0;
        const auto listed = data.filter_items.find(in.filter);
        if (listed != data.filter_items.end() && !listed->second.empty()) pick = listed->second.front();
        if (pick == 0) pick = item_with_tags(data, crafting::split_tags(in.tags));
        if (pick == 0) continue;
        const int q = quality_at_least(world, pick, in.min_rarity);
        s.items.push_back(farm_item(slot++, pick, static_cast<int>(need)));
        s.items.back().quality = q;
        s.req.tags.push_back(pick_of({resource(pick, static_cast<int>(need), world.item_rarity_facts(pick).rarity_of(q))}, crafting::split_tags(in.tags)));
    }
    for (const auto& [key, count] : named) {
        bool merged = false;
        for (repl::CraftResource& r : s.req.general) {
            if (r.record != key.first) continue;
            bool same = false;
            for (repl::CraftRarityCount& c : r.rarities) {
                if (c.rarity == key.second) {
                    c.count += count;
                    same = true;
                }
            }
            if (!same) r.rarities.push_back({static_cast<uint8_t>(key.second), static_cast<int32_t>(count), false});
            merged = true;
            break;
        }
        if (!merged) s.req.general.push_back(resource(key.first, static_cast<int>(count), key.second));
    }
    return s;
}

/// The facts a Cooking start at a station reads: the station, a tier, the bags and the purse; `taught` says whether a learnable recipe is known.
static crafting::StartFacts cook_facts(const content::CraftRecipeData& data, const content::CraftStationDef& station, const items::Inventory& bag, const PlayerWorld& world, int tier,
                                       bool taught) {
    crafting::StartFacts f;
    f.recipes = &data;
    f.station = &station;
    f.tier_of = [tier](int64_t) { return tier; };
    f.taught = [taught](int64_t) { return taught; };
    f.inventory = &bag;
    f.rarity_facts = [&world](int64_t item) { return world.item_rarity_facts(item); };
    f.copper = 1'000'000;
    f.node.level = 3;
    return f;
}

/// The real file's recipes and the two stations; the baits' filter is marked as a contentc-47 file's table would (it lists no item, so the tags decide).
static bool cooking_data(const content::ContentDb& db, content::CraftRecipeData& data, const content::CraftStationData& stations, const content::CraftStationDef*& t1,
                         const content::CraftStationDef*& t2) {
    std::string error;
    auto loaded = db.craft_recipe_data(error);
    if (!loaded) {
        std::printf("  [FAIL] the recipes do not read: %s\n", error.c_str());
        ++g_fail;
        return false;
    }
    data = std::move(*loaded);
    bool filtered = false;
    for (const auto& [guid, recipe] : data.recipes) {
        for (const content::CraftInput& in : recipe.inputs) {
            if (in.item == 0 && in.filter != 0 && data.filter_items.count(in.filter) == 0) filtered = true;
        }
    }
    if (filtered) data.filters_read = true;
    t1 = station_named(stations, "ProcessingStation_Cooking_T1");
    t2 = station_named(stations, "ProcessingStation_Cooking_T2");
    return t1 && t2;
}

/// The Cooking stations, their tiers and the 81 recipes; every Novice and Apprentice recipe starts and runs.
static void test_cooking_recipes(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("the Cooking station's 81 Novice and Apprentice recipes\n");
    std::string error;
    const auto station_data = db.craft_station_data(error);
    if (!station_data) {
        std::printf("  [FAIL] the station definitions do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    content::CraftRecipeData data;
    const content::CraftStationDef* t1 = nullptr;
    const content::CraftStationDef* t2 = nullptr;
    if (!cooking_data(db, data, *station_data, t1, t2)) return;
    CHECK_SHOWN(t2->display_name == "Cooking - Apprentice" && t2->max_tier == 1, "the live Village Marketplace station is Cooking - Apprentice (tier 1 at most)");
    CHECK_SHOWN(t1->display_name == "Cooking - Novice" && t1->max_tier == 0, "the Encampment station is Cooking - Novice (tier 0 at most)");

    std::vector<const content::CraftRecipe*> recipes;
    int novice = 0, apprentice = 0;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != cook::kCooking) continue;
        if (!data.set_holds(t2->recipe_set, guid)) continue;
        recipes.push_back(&r);
        if (r.tier == 0) ++novice;
        if (r.tier == 1) ++apprentice;
    }
    CHECK_SHOWN(recipes.size() == 81 && novice == 22 && apprentice == 59, "Cooking - Apprentice holds 81 recipes: 22 Novice and 59 Apprentice");

    size_t startable = 0, t0 = 0, t1n = 0, timed = 0, paid = 0, products = 0;
    const content::CraftRecipe* sample_novice = nullptr;
    const content::CraftRecipe* sample_apprentice = nullptr;
    const auto first_product = [&](const content::CraftRecipe& r) {
        const content::CraftReward* reward = data.reward_of(r);
        if (!reward) return int64_t{0};
        for (const content::CraftRewardContainer& box : reward->containers)
            for (const std::vector<content::CraftRewardItem>& units : box.rewards)
                for (const content::CraftRewardItem& it : units)
                    if (it.item != 0) return it.item;
        return int64_t{0};
    };
    for (const content::CraftRecipe* r : recipes) {
        CookStart start = cooking_start(data, *r, world, 1);
        auto bag = bag_of(start.items);
        if (!bag) {
            std::printf("  [FAIL] %s: its bag does not read\n", r->name.c_str());
            ++g_fail;
            continue;
        }
        const cr::Plan plan = cr::plan_start(start.req, cook_facts(data, *t2, *bag, world, r->tier, /*taught=*/true));
        if (!plan.ok) {
            std::printf("  [FAIL] %s does not start: %s\n", r->name.c_str(), plan.refusal.c_str());
            ++g_fail;
            continue;
        }
        ++startable;
        if (r->tier == 0) ++t0;
        if (r->tier == 1) ++t1n;
        if (r->tier == 0) sample_novice = r;
        if (r->tier == 1) sample_apprentice = r;
        const int64_t want_ms = static_cast<int64_t>(std::llround(r->base_duration * 1000.0));
        if (plan.duration_ms != want_ms || plan.takes.empty() || plan.cost < 0) {
            std::printf("  [FAIL] %s: %lld ms (want %lld), %zu takes, %lld copper\n", r->name.c_str(), static_cast<long long>(plan.duration_ms),
                        static_cast<long long>(want_ms), plan.takes.size(), static_cast<long long>(plan.cost));
            ++g_fail;
        } else {
            ++timed;
        }
        svc::ProcessingJob job;
        job.recipe_guid = r->guid;
        job.quantity = 1;
        job.resources = cr::job_resources_of(plan.paid);
        job.job_guid = "00000000000000000000000000000012";
        const auto rolled = cr::roll_job(data, job, 25, cr::Ratings{}, [&](int64_t item) { return world.item_rarity_facts(item); });
        const int64_t reward_item = first_product(*r);
        if (rolled && rolled->profession == cook::kCooking && rolled->profession_xp >= (r->tier == 0 ? 50 : 60) && !rolled->products.empty() && rolled->products.front().item == reward_item) {
            ++paid;
            if (!rolled->products.empty() && rolled->products.front().quantity >= 1) ++products;
        } else {
            std::printf("  [FAIL] %s: the job pays %lld Cooking experience and makes %zu product(s)\n", r->name.c_str(),
                        rolled ? static_cast<long long>(rolled->profession_xp) : -1ll, rolled ? rolled->products.size() : 0);
            ++g_fail;
        }
    }
    CHECK_SHOWN(startable == 81, "all 81 Novice and Apprentice recipes start at Cooking - Apprentice with the game's own inputs");
    CHECK_SHOWN(t0 == 22 && t1n == 59, "the 22 Novice and the 59 Apprentice recipes each start");
    CHECK_SHOWN(timed == 81, "each job takes its inputs and runs its recipe's own base time");
    CHECK_SHOWN(paid == 81 && products == 81, "each job pays its profession's Cooking experience and makes the product its reward table names");

    // the rest of the profession's station reach: the 74 recipes only the Journeyman station holds, the 20 that have to be learned, the 2 baits' filter
    const content::CraftStationDef* t3 = station_named(*station_data, "ProcessingStation_Cooking_T3");
    size_t journeyman = 0, learnable = 0, baits = 0;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != cook::kCooking) continue;
        if (t3 && data.set_holds(t3->recipe_set, guid) && !data.set_holds(t2->recipe_set, guid)) ++journeyman;
        if (r.learnable && r.tier <= 1) ++learnable;
        if (!data.set_holds(t2->recipe_set, guid)) continue;
        for (const content::CraftInput& in : r.inputs) {
            if (in.item == 0 && in.filter != 0) ++baits;   // (a filter input among the 81: the two baits)
        }
    }
    CHECK_SHOWN(t3 && t3->max_tier == 2 && journeyman == 74, "the 74 recipes above Apprentice are only in the Journeyman station's set (held back with the Cookhouse)");
    CHECK_SHOWN(learnable == 20, "20 of the 81 have to be learned: 2 Novice and 18 Apprentice recipes");
    CHECK_SHOWN(baits == 2, "2 of the 81 take a gather-item filter input (the freshwater and saltwater bait)");

    // the profession's experience and the product, on a Novice and an Apprentice recipe
    CHECK_SHOWN(sample_novice && sample_apprentice, "a Novice and an Apprentice recipe are found for the payout");
    if (sample_novice && sample_apprentice) {
        const auto roll = [&](const content::CraftRecipe& r, int64_t units, std::vector<svc::JobResource> resources) {
            svc::ProcessingJob job;
            job.recipe_guid = r.guid;
            job.quantity = units;
            job.resources = std::move(resources);
            job.job_guid = "00000000000000000000000000000011";
            return cr::roll_job(data, job, 25, cr::Ratings{}, [&](int64_t item) { return world.item_rarity_facts(item); });
        };
        const auto product_of = [&](const content::CraftRecipe& r) {
            const content::CraftReward* reward = data.reward_of(r);
            int64_t item = 0;
            if (reward)
                for (const content::CraftRewardContainer& box : reward->containers)
                    for (const std::vector<content::CraftRewardItem>& units : box.rewards)
                        for (const content::CraftRewardItem& it : units)
                            if (item == 0) item = it.item;
            return item;
        };
        const int64_t novice_product = product_of(*sample_novice);
        const int64_t apprentice_product = product_of(*sample_apprentice);
        const svc::JobResource novice_paid{sample_novice->inputs.front().item, {{0, sample_novice->inputs.front().quantity, false}}};
        const svc::JobResource apprentice_paid{sample_apprentice->inputs.front().item, {{0, sample_apprentice->inputs.front().quantity, false}}};
        const auto novice_roll = roll(*sample_novice, 1, {novice_paid});
        const auto apprentice_roll = roll(*sample_apprentice, 1, {apprentice_paid});
        CHECK_SHOWN(novice_roll && novice_roll->profession == cook::kCooking && novice_roll->profession_xp == 50 && novice_roll->products.size() == 1 &&
                        novice_roll->products.front().item == novice_product,
                    "a Novice recipe pays 50 Cooking experience a unit at Common and makes its product");
        CHECK_SHOWN(apprentice_roll && apprentice_roll->profession == cook::kCooking && apprentice_roll->profession_xp == 60 && apprentice_roll->products.size() == 1 &&
                        apprentice_roll->products.front().item == apprentice_product,
                    "an Apprentice recipe pays 60 Cooking experience a unit at Common and makes its product");
    }
    SAYOK("the Cooking station's 81 recipes");
}

/// The station's tier and the certification that open the Apprentice recipes; the recipe that has to be learned; the filter input.
static void test_cooking_conditions(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("the checks that decide a Cooking start\n");
    std::string error;
    const auto station_data = db.craft_station_data(error);
    if (!station_data) {
        std::printf("  [FAIL] the station definitions do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    content::CraftRecipeData data;
    const content::CraftStationDef* t1 = nullptr;
    const content::CraftStationDef* t2 = nullptr;
    if (!cooking_data(db, data, *station_data, t1, t2)) return;
    const content::CraftRecipe* spice = recipe_named(data, "Cooking_T1_Ingredient_AelanSpice");
    const content::CraftRecipe* bait = recipe_named(data, "Cooking_T1_Bait_Chum_Freshwater");
    const content::CraftRecipe* bork = recipe_named(data, "Cooking_T2_Meat_Bork");
    const content::CraftRecipe* bread = recipe_named(data, "Cooking_T2_Food_Bread");
    CHECK_SHOWN(spice && bait && bork && bread, "the Novice spice, the fish bait and two Apprentice recipes are in the data");
    if (!spice || !bait || !bork || !bread) return;

    {   // the station's tier: a tier-1 recipe at a Novice-tier station is refused; the same set at the Apprentice tier starts it
        CookStart start = cooking_start(data, *bread, world, 1);
        auto bag = bag_of(start.items);
        CHECK_SHOWN(bag.has_value(), "a bag of the Bread inputs");
        if (bag) {
            content::CraftStationDef novice_tier = *t2;   // the Apprentice station's set, at a Novice tier (the data's own station sets hold one tier only)
            novice_tier.max_tier = 0;
            const cr::Plan novice_station = cr::plan_start(start.req, cook_facts(data, novice_tier, *bag, world, 1, true));
            const cr::Plan apprentice_station = cr::plan_start(start.req, cook_facts(data, *t2, *bag, world, 1, true));
            CHECK_SHOWN(!novice_station.ok && novice_station.refusal.find("Cooking - Apprentice reaches 0") != std::string::npos,
                        "an Apprentice recipe is refused at a station that reaches tier 0, named as the window");
            CHECK_SHOWN(apprentice_station.ok, "the same recipe starts at Cooking - Apprentice");
            const cr::Plan set_refused = cr::plan_start(start.req, cook_facts(data, *t1, *bag, world, 1, true));
            CHECK_SHOWN(!set_refused.ok && set_refused.refusal.find("is not one of the recipes of Cooking - Novice") != std::string::npos,
                        "the Novice station's set does not hold the Apprentice recipes at all, and the refusal names the window");
        }
    }
    {   // the certification: a Novice's start is refused at the Apprentice station, an Apprentice's is not
        CookStart start = cooking_start(data, *bread, world, 1);
        auto bag = bag_of(start.items);
        if (bag) {
            const cr::Plan novice = cr::plan_start(start.req, cook_facts(data, *t2, *bag, world, 0, true));
            const cr::Plan apprentice = cr::plan_start(start.req, cook_facts(data, *t2, *bag, world, 1, true));
            CHECK_SHOWN(!novice.ok && novice.refusal.find("needs certification tier 1") != std::string::npos, "a Novice's Apprentice recipe is refused for the certification");
            CHECK_SHOWN(apprentice.ok, "the Apprentice certification opens the same recipe at the same station");
        }
    }
    {   // a recipe that has to be learned: refused until it is taught, then it starts
        CookStart start = cooking_start(data, *bork, world, 1);
        auto bag = bag_of(start.items);
        if (bag) {
            const cr::Plan not_taught = cr::plan_start(start.req, cook_facts(data, *t2, *bag, world, 1, false));
            const cr::Plan taught = cr::plan_start(start.req, cook_facts(data, *t2, *bag, world, 1, true));
            CHECK_SHOWN(!not_taught.ok && not_taught.refusal.find("has to be learned") != std::string::npos, "Butcher Bork is refused until its recipe is taught");
            CHECK_SHOWN(taught.ok, "the taught recipe starts at the same station");
        }
    }
    {   // the filter input: 50 fish of the tag start the bait; 50 Daffodil do not; 49 fish fall short
        CookStart start = cooking_start(data, *bait, world, 1);
        auto bag = bag_of(start.items);
        CHECK_SHOWN(bag.has_value() && start.req.tags.size() == 1, "the bait names its fish pick in a tag selection");
        if (bag) {
            const cr::Plan fish = cr::plan_start(start.req, cook_facts(data, *t2, *bag, world, 0, true));
            CHECK_SHOWN(fish.ok, "a Novice starts the freshwater bait chum at Cooking - Apprentice with 50 fish of the fish tag");
            if (!fish.ok) std::printf("    %s\n", fish.refusal.c_str());
            CookStart wrong = cooking_start(data, *bait, world, 1);
            wrong.req.tags = {pick_of({resource(cook::kDaffodil, 50)}, {"Item.Resource.Raw.Fish"})};
            auto plant_bag = bag_of({farm_item(0, cook::kDaffodil, 50)});
            const cr::Plan plant = plant_bag ? cr::plan_start(wrong.req, cook_facts(data, *t2, *plant_bag, world, 0, true)) : cr::Plan{};
            CHECK_SHOWN(!plant.ok && plant.refusal.find("does not carry the tag item.resource.raw.fish") != std::string::npos, "the bait chum refuses 50 Daffodil: they carry no fish tag");
            CookStart short_pick = cooking_start(data, *bait, world, 1);
            short_pick.req.tags = {pick_of({resource(cook::kBluegill, 49)}, {"Item.Resource.Raw.Fish"})};
            auto short_bag = bag_of({farm_item(0, cook::kBluegill, 49)});
            const cr::Plan short_plan = short_bag ? cr::plan_start(short_pick.req, cook_facts(data, *t2, *short_bag, world, 0, true)) : cr::Plan{};
            CHECK_SHOWN(!short_plan.ok && short_plan.refusal.find("names 49 items for the tags") != std::string::npos, "49 fish are one short of the bait's 50");
        }
    }
    {   // the same start twice, and a second player at the same station: planning takes nothing; the first job's takes empty the bag
        CookStart start = cooking_start(data, *spice, world, 1);
        auto bag = bag_of(start.items);
        CHECK_SHOWN(bag.has_value(), "a bag of the Aelan Spice inputs");
        if (bag) {
            const cr::Plan first = cr::plan_start(start.req, cook_facts(data, *t2, *bag, world, 0, true));
            const int64_t pepper = spice->inputs.back().item;
            CHECK_SHOWN(first.ok && bag->count_materials(pepper, world.item_rarity_facts(pepper), 0, items::kRarityCount - 1) > 0,
                        "planning a job takes nothing from the bags (a second player's bag stays his)");
            bool applied = first.ok;
            for (const cr::Take& t : first.takes) {
                applied = bag->take_materials(t.record, t.facts, t.rarity_lo, t.rarity_hi, t.count, t.lowest_first).ok && applied;
            }
            const cr::Plan again = cr::plan_start(start.req, cook_facts(data, *t2, *bag, world, 0, true));
            CHECK_SHOWN(applied && first.ok && !again.ok, "the same job started twice: the second finds the materials the first took gone");
            CookStart other = cooking_start(data, *spice, world, 1);
            auto other_bag = bag_of(other.items);
            const cr::Plan second_player = other_bag ? cr::plan_start(other.req, cook_facts(data, *t2, *other_bag, world, 0, true)) : cr::Plan{};
            CHECK_SHOWN(second_player.ok, "a second player with his own inputs starts the same job at the same station");
            const std::vector<svc::JobResource> saved = cr::job_resources_of(first.paid);
            const svc::ProcessingJob job = cr::make_job({}, "0123456789abcdef", spice->guid, 1, first.duration_ms, {1, 2, 3, 4}, saved, 1000);
            const auto state = cr::to_state(job, 5000, 10.0);
            CHECK_SHOWN(state.has_value() && !state->general.empty(), "the job saves what it was paid with, so a reconnect shows it");
        }
    }
    {   // the station's queue: two jobs at the station fill it, the third is refused; a job of the most units the station takes runs its time
        CookStart one = cooking_start(data, *spice, world, 1);
        auto one_bag = bag_of(one.items);
        if (one_bag) {
            crafting::StartFacts full = cook_facts(data, *t2, *one_bag, world, 0, true);
            full.unfinished_here = static_cast<size_t>(std::max(1, t2->queue_size));
            const cr::Plan queued = cr::plan_start(one.req, full);
            CHECK_SHOWN(!queued.ok && queued.refusal.find("the queue at Cooking - Apprentice is full") != std::string::npos, "a third job at the station's two-slot queue is refused");
        }
        CookStart many = cooking_start(data, *spice, world, 250);
        auto many_bag = bag_of(many.items);
        if (many_bag) {
            const cr::Plan plan = cr::plan_start(many.req, cook_facts(data, *t2, *many_bag, world, 0, true));
            CHECK_SHOWN(plan.ok && plan.duration_ms == 15'000'000 && plan.takes.size() == 3, "a 250-unit Novice job starts, takes 750 items from 3 records and runs its 15,000 s");
        }
    }
    {   // the recipe item: the data's item teaches the data's recipe, and that is what makes the recipe start
        const auto learn = db.item_learn(cook::kFriedBluegillRecipeItem, error);
        CHECK_SHOWN(learn.has_value() && !learn->empty(), "Consumable_Recipe_Cooking_T2_FriedBluegill teaches a recipe in item_learn");
        if (learn && !learn->empty()) {
            const auto it = std::find_if(learn->begin(), learn->end(), [](const content::ItemLearnRow& row) { return row.recipe == cook::kFriedBluegill; });
            CHECK_SHOWN(it != learn->end(), "the item teaches Cooking_T2_Food_FriedBluegill");
            if (it != learn->end()) {
                CHECK_SHOWN(it->profession == cook::kCooking && it->certification == cook::kApprenticeCert, "the item asks for Cooking and its Apprentice certification");
            }
        }
    }
    SAYOK("the checks that decide a Cooking start");
}

/// The Cooking - Novice station (tier 0 at most; the artisan buildings and the Encampment's marketplace place it): its 22 recipes start with the reagents a shop sells, and nothing above them does.
static void test_cooking_novice_station(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("the Cooking - Novice station's 22 recipes\n");
    std::string error;
    const auto station_data = db.craft_station_data(error);
    if (!station_data) {
        std::printf("  [FAIL] the station definitions do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    content::CraftRecipeData data;
    const content::CraftStationDef* t1 = nullptr;
    const content::CraftStationDef* t2 = nullptr;
    if (!cooking_data(db, data, *station_data, t1, t2)) return;
    std::vector<const content::CraftRecipe*> novice;
    size_t also_apprentice = 0;
    for (const auto& [guid, r] : data.recipes) {
        if (r.profession != cook::kCooking || !data.set_holds(t1->recipe_set, guid)) continue;
        novice.push_back(&r);
        if (data.set_holds(t2->recipe_set, guid)) ++also_apprentice;
    }
    CHECK_SHOWN(novice.size() == 22 && also_apprentice == 22, "the Novice station holds 22 Cooking recipes, and the Apprentice station holds the same 22");
    size_t tier0 = 0, started = 0;
    for (const content::CraftRecipe* r : novice) {
        if (r->tier == 0) ++tier0;
        CookStart start = cooking_start(data, *r, world, 1, &db);
        auto bag = bag_of(start.items);
        if (!bag) {
            std::printf("  [FAIL] %s: its bag does not read\n", r->name.c_str());
            ++g_fail;
            continue;
        }
        const cr::Plan plan = cr::plan_start(start.req, cook_facts(data, *t1, *bag, world, 0, /*taught=*/true));
        if (plan.ok) {
            ++started;
        } else {
            std::printf("  [FAIL] %s does not start at Cooking - Novice: %s\n", r->name.c_str(), plan.refusal.c_str());
            ++g_fail;
        }
    }
    CHECK_SHOWN(tier0 == 22 && started == 22, "all 22 Novice recipes start at Cooking - Novice for a Novice, with the reagents a shop sells");

    const content::CraftRecipe* bread = recipe_named(data, "Cooking_T2_Food_Bread");
    const content::CraftRecipe* bear = recipe_named(data, "Cooking_T1_Meat_Bear");
    CHECK_SHOWN(bread && bear, "an Apprentice recipe and a Novice recipe that has to be learned are in the data");
    if (!bread || !bear) return;
    {   // an Apprentice (or better) at the Novice station: the recipe is not one of its recipes, and the log names the window
        CookStart start = cooking_start(data, *bread, world, 1, &db);
        auto bag = bag_of(start.items);
        if (bag) {
            const cr::Plan plan = cr::plan_start(start.req, cook_facts(data, *t1, *bag, world, 2, /*taught=*/true));
            CHECK_SHOWN(!plan.ok && plan.refusal.find("is not one of the recipes of Cooking - Novice") != std::string::npos, "a Journeyman's Apprentice recipe is refused at Cooking - Novice, named as the window");
        }
    }
    {   // a Novice recipe that has to be learned, at the Novice station: refused until it is taught
        CookStart start = cooking_start(data, *bear, world, 1, &db);
        auto bag = bag_of(start.items);
        if (bag) {
            const cr::Plan not_taught = cr::plan_start(start.req, cook_facts(data, *t1, *bag, world, 0, /*taught=*/false));
            const cr::Plan taught = cr::plan_start(start.req, cook_facts(data, *t1, *bag, world, 0, /*taught=*/true));
            CHECK_SHOWN(!not_taught.ok && not_taught.refusal.find("has to be learned") != std::string::npos, "Bear Meat is refused at Cooking - Novice until its recipe is taught");
            CHECK_SHOWN(taught.ok, "the taught Bear Meat starts at Cooking - Novice");
        }
    }
    {   // the Novice station's queue holds one job (the Apprentice station's two): a second job waits until the first is collected
        const content::CraftRecipe* spice = recipe_named(data, "Cooking_T1_Ingredient_AelanSpice");
        CHECK_SHOWN(spice && t1->queue_size == 1 && t2->queue_size == 2, "the Novice station's queue is 1 job and the Apprentice station's is 2");
        if (spice) {
            CookStart start = cooking_start(data, *spice, world, 1, &db);
            auto bag = bag_of(start.items);
            if (bag) {
                crafting::StartFacts busy = cook_facts(data, *t1, *bag, world, 0, true);
                busy.unfinished_here = 1;
                const cr::Plan plan = cr::plan_start(start.req, busy);
                CHECK_SHOWN(!plan.ok && plan.refusal.find("the queue at Cooking - Novice is full (1 of 1)") != std::string::npos, "a second job at Cooking - Novice is refused while the first is not over");
            }
        }
    }
    {   // a station that makes nothing of Cooking's: the refusal for a crafting station's recipe also names the window, and a quick start at a processing station says so
        CookStart start = cooking_start(data, *bread, world, 1, &db);
        auto bag = bag_of(start.items);
        if (bag) {
            crafting::StartFacts quick = cook_facts(data, *t2, *bag, world, 1, true);
            quick.quick = true;
            const cr::Plan plan = cr::plan_start(start.req, quick);
            CHECK_SHOWN(!plan.ok && plan.refusal.find("Cooking - Apprentice is a processing station") != std::string::npos, "a quick make at a processing station is refused, naming the window");
        }
    }
    SAYOK("the Cooking - Novice station");
}

/// The reagents a shop sells and the jobs a character holds: every one of the 81 recipes starts with its reagents at the quality the Marketplace sells them at, and the bait's job (the
/// picks of its filter input included) is what a reconnect shows, at each moment of its time, behind a job that came first at the same station, and with a second player's own queue.
static void test_cooking_shop_and_saved_jobs(const content::ContentDb& db, const PlayerWorld& world) {
    namespace cr = aoc::worldd::crafting;
    std::printf("Cooking with shop-bought reagents, and the jobs a reconnect shows\n");
    std::string error;
    const auto station_data = db.craft_station_data(error);
    if (!station_data) {
        std::printf("  [FAIL] the station definitions do not read: %s\n", error.c_str());
        ++g_fail;
        return;
    }
    content::CraftRecipeData data;
    const content::CraftStationDef* t1 = nullptr;
    const content::CraftStationDef* t2 = nullptr;
    if (!cooking_data(db, data, *station_data, t1, t2)) return;

    {   // the Marketplace sells each reagent at its default quality (1): a recipe that asks for a rarity (Bonefire Salt's Uncommon) is met by what the shop sold
        size_t recipes = 0, started = 0, shop_reagents = 0, rarity_asked = 0;
        for (const auto& [guid, r] : data.recipes) {
            if (r.profession != cook::kCooking || !data.set_holds(t2->recipe_set, guid)) continue;
            ++recipes;
            for (const content::CraftInput& in : r.inputs) {
                if (!in.primary) continue;
                ++shop_reagents;
                if (in.min_rarity > 0) ++rarity_asked;
            }
            CookStart start = cooking_start(data, r, world, 1, &db);
            auto bag = bag_of(start.items);
            if (!bag) continue;
            const cr::Plan plan = cr::plan_start(start.req, cook_facts(data, *t2, *bag, world, r.tier, /*taught=*/true));
            if (plan.ok) {
                ++started;
            } else {
                std::printf("  [FAIL] %s does not start with shop-bought reagents: %s\n", r.name.c_str(), plan.refusal.c_str());
                ++g_fail;
            }
        }
        CHECK_SHOWN(recipes == 81 && started == 81, "all 81 recipes start with their reagents at the quality a shop sells");
        CHECK_SHOWN(shop_reagents == 34 && rarity_asked == 6, "34 of their inputs are reagents, 6 of which (Bonefire Salt) ask for Uncommon: a shop's quality 1 meets it");
    }

    const content::CraftRecipe* bait = recipe_named(data, "Cooking_T1_Bait_Chum_Freshwater");
    CHECK_SHOWN(bait != nullptr, "Freshwater Chum is in the data");
    if (!bait) return;
    CookStart start = cooking_start(data, *bait, world, 1, &db);
    auto bag = bag_of(start.items);
    CHECK_SHOWN(bag.has_value(), "a bag of 50 fish");
    if (!bag) return;
    const cr::Plan plan = cr::plan_start(start.req, cook_facts(data, *t2, *bag, world, 0, true));
    CHECK_SHOWN(plan.ok && plan.duration_ms == 60'000, "the bait starts and runs 60 s");
    if (!plan.ok) return;

    constexpr const char* kKey = "0123456789abcdef";
    const std::vector<svc::JobResource> saved = cr::job_resources_of(plan.paid);
    const svc::ProcessingJob first = cr::make_job({}, kKey, bait->guid, 1, plan.duration_ms, {1, 2, 3, 4}, saved, 1000);
    const auto fish_of = [](const repl::CraftItemState& s) {
        int64_t n = 0;
        for (const repl::CraftResource& r : s.general)
            if (r.record == cook::kBluegill)
                for (const repl::CraftRarityCount& c : r.rarities) n += c.count;
        return n;
    };
    {   // a reconnect while it runs: the same job, the 50 fish it was paid with and the time left on the new session's clock
        const auto running = cr::to_state(first, 30'000, 100.0);
        CHECK_SHOWN(running && running->status == cr::kInProgress && running->quantity == 1 && fish_of(*running) == 50, "the running bait job shows the 50 fish it was paid with");
        const auto later = cr::to_state(first, 45'000, 500.0);   // logged out and in 15 s later, on a clock that reads 500
        CHECK_SHOWN(later && later->status == cr::kInProgress && later->start_time + static_cast<double>(later->duration) - 500.0 == 16.0, "after a reconnect it has 16 of its 60 s left");
        CHECK_SHOWN(cr::status_at(first, 61'001) == cr::kComplete && cr::unfinished_at({first}, kKey, 61'001) == 0, "it is complete after its 60 s and counts no more against the queue");
    }
    {   // a job that comes second at the station waits for the first, and is pending on the reconnect
        const svc::ProcessingJob second = cr::make_job({first}, kKey, bait->guid, 1, plan.duration_ms, {5, 6, 7, 8}, saved, 2000);
        CHECK_SHOWN(second.started_at_ms == first.end_ms(), "the second bait job begins when the first ends");
        const auto waiting = cr::to_state(second, 30'000, 100.0);
        CHECK_SHOWN(waiting && waiting->status == cr::kPending && fish_of(*waiting) == 50, "the second job is pending, with its own 50 fish");
        CHECK_SHOWN(cr::unfinished_at({first, second}, kKey, 30'000) == 2 && cr::held_at({first, second}, kKey) == 2, "both count against the station's two-slot queue");
        CHECK_SHOWN(cr::unfinished_at({first, second}, kKey, 130'000) == 0 && cr::held_at({first, second}, kKey) == 2, "after their time both are held until they are collected");
    }
    {   // a finished job shows what it made and pays: the chum and the 50 experience
        svc::ProcessingJob job = first;
        job.job_guid = "00000000000000000000000000000013";
        const auto rolled = cr::roll_job(data, job, 25, cr::Ratings{}, [&](int64_t item) { return world.item_rarity_facts(item); });
        CHECK_SHOWN(rolled && !rolled->products.empty() && rolled->profession == cook::kCooking && rolled->profession_xp == 50, "the finished bait job makes its product and pays 50 Cooking experience");
        if (rolled) {
            const auto done = cr::to_state(job, 70'000, 100.0, &*rolled);
            CHECK_SHOWN(done && done->status == cr::kComplete && !done->products.empty() && fish_of(*done) == 50, "a reconnect after the end shows it complete, with its products and the fish it was paid with");
        }
    }
    {   // a second player at the same station has his own queue: the first player's full queue refuses him nothing, and his job takes from his own bag
        crafting::StartFacts first_player = cook_facts(data, *t2, *bag, world, 0, true);
        first_player.unfinished_here = static_cast<size_t>(std::max(1, t2->queue_size));
        CookStart other = cooking_start(data, *bait, world, 1, &db);
        auto other_bag = bag_of(other.items);
        const cr::Plan refused = cr::plan_start(start.req, first_player);
        const cr::Plan second_player = other_bag ? cr::plan_start(other.req, cook_facts(data, *t2, *other_bag, world, 0, true)) : cr::Plan{};
        CHECK_SHOWN(!refused.ok && second_player.ok, "the player whose queue is full is refused, and a second player at the same station starts the same job");
    }
    SAYOK("Cooking with shop-bought reagents, and the jobs a reconnect shows");
}
