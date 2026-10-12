// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_spawn_filter.cpp  (plan E1, stage 3, part 1)
//
//  The spawn filter (spawns/spawn_filter.h): which of the design data's spawn points retail spawns for a character in this area now.
//
//    the conditions   a PopulationFilter against the context (node level, culture, node ids and zones, node type, biome, season, weather, the time of day
//                     with its wrap past midnight, the required tags), an override replacing the asset's filter, the point's rules (dev only, the map,
//                     blacklists, the gates and the sources a test turns on), the Blueprint's visuals (hidden, placeholder, no mesh, no animation, a
//                     humanoid whose body needs an appearance)
//    the table        npc_motion.tsv's new columns (13, 17 and 21 columns read), and the retail captures as the answer key: of the Blueprints the captures
//                     open, an NPCAppearanceComponent comes with the player's master mesh and with nothing else
//    the real area    content.db (contentc version 9) and npc_motion.tsv: no point of another map spawns, no humanoid with no look, none of the stage-2 run's
//                     invisible villagers, quest masters and T-posed Wail; what retail's captures show in the area (Goblin_Bonebinder, Wolf) spawns; the
//                     kill switch gives stage 2's area back; every point is counted once, by reason
// ============================================================================
#include "content/content_db.h"
#include "npc/motion_table.h"
#include "spawns/area_loader.h"
#include "spawns/captured_spawns.h"
#include "spawns/generic_npc.h"
#include "spawns/population.h"
#include "spawns/spawn_filter.h"
#include "npc_appearance_key.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace aoc::worldd;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

using spawns::SpawnContext;
using spawns::SpawnFilter;
using spawns::Why;

static content::PopFilter open_filter() { return content::PopFilter{}; }   // Wilderness .. Metropolis, nothing else
// [Cyb - C-O-A]

static void test_conditions() {
    std::printf("PopulationFilter against the context\n");
    SpawnContext ctx;
    SpawnFilter f(ctx, nullptr);
    const std::set<std::string> none;
    CHECK(f.filter(open_filter(), none) == Why::Spawns, "an open filter passes");

    content::PopFilter v = open_filter();
    v.node_level_min = v.node_level_max = "Village";
    CHECK(f.filter(v, none) == Why::NodeLevel, "a Village-only asset does not spawn in a world with no node (Wilderness)");
    SpawnContext village = ctx;
    village.node_level = "Village";
    CHECK(SpawnFilter(village, nullptr).filter(v, none) == Why::Spawns, "...and does at Village level");
    content::PopFilter range = open_filter();
    range.node_level_min = "Encampment";
    range.node_level_max = "Metropolis";
    CHECK(f.filter(range, none) == Why::NodeLevel && SpawnFilter(village, nullptr).filter(range, none) == Why::Spawns, "a range counts the levels between its ends");
    content::PopFilter never = open_filter();
    never.node_level_min = never.node_level_max = "cCount";
    CHECK(f.filter(never, none) == Why::NodeLevel && SpawnFilter(village, nullptr).filter(never, none) == Why::NodeLevel, "cCount bounds are the data's \"never\"");
    CHECK(SpawnFilter::node_level_number("Wilderness") == 0 && SpawnFilter::node_level_number("Crossroads") == 1 && SpawnFilter::node_level_number("Metropolis") == 6 &&
              SpawnFilter::node_level_number("cCount") == 7 && SpawnFilter::node_level_number("Capital") == -1,
          "the node levels' order is the game's (Wilderness, Crossroads, Encampment, Village, Town, City, Metropolis)");

    content::PopFilter culture = open_filter();
    culture.cultures = {"Dunir"};
    CHECK(f.filter(culture, none) == Why::Culture, "a Dunir asset does not spawn in the Kaelar area");
    culture.cultures = {"Dunir", "Kaelar"};
    CHECK(f.filter(culture, none) == Why::Spawns, "...a list that holds Kaelar does");

    content::PopFilter node = open_filter();
    node.node_ids = {42};
    CHECK(f.filter(node, none) == Why::NodeOrZone, "an asset of a node this area has none of");
    SpawnContext with_node = ctx;
    with_node.node_id = 42;
    CHECK(SpawnFilter(with_node, nullptr).filter(node, none) == Why::Spawns, "...unless the context names the node");
    content::PopFilter zone = open_filter();
    zone.zones = {7};
    CHECK(f.filter(zone, none) == Why::NodeOrZone, "a zone of influence the area has none of");

    content::PopFilter other = open_filter();
    other.node_types = {"Scientific"};
    CHECK(f.filter(other, none) == Why::NodeType, "a node type");
    other = open_filter();
    other.biomes = {"FloodPlains"};
    CHECK(f.filter(other, none) == Why::Biome, "a biome the context does not know");
    SpawnContext biome = ctx;
    biome.biome = "FloodPlains";
    CHECK(SpawnFilter(biome, nullptr).filter(other, none) == Why::Spawns, "...and one it does");
    other = open_filter();
    other.seasons = {"Winter"};
    CHECK(f.filter(other, none) == Why::Season, "a season");
    other.seasons = {"Winter", "Summer"};
    CHECK(f.filter(other, none) == Why::Spawns, "...Summer is the default");
    other = open_filter();
    other.weather = {"Rain"};
    CHECK(f.filter(other, none) == Why::Weather, "a weather (the sky is clear)");

    content::PopFilter day = open_filter();
    day.time_of_day_start = 10;
    day.time_of_day_end = 23;
    SpawnContext at = ctx;
    at.hour = 12;
    CHECK(SpawnFilter(at, nullptr).filter(day, none) == Why::Spawns, "a 10-23 asset spawns at noon");
    at.hour = 2;
    CHECK(SpawnFilter(at, nullptr).filter(day, none) == Why::TimeOfDay, "...not at 2 in the morning");
    day.time_of_day_start = 22;
    day.time_of_day_end = 5;
    at.hour = 23.5;
    const bool late = SpawnFilter(at, nullptr).filter(day, none) == Why::Spawns;
    at.hour = 3;
    const bool early = SpawnFilter(at, nullptr).filter(day, none) == Why::Spawns;
    at.hour = 12;
    CHECK(late && early && SpawnFilter(at, nullptr).filter(day, none) == Why::TimeOfDay, "a night asset (22 to 5) wraps past midnight");
    day.time_of_day_start = 0;
    day.time_of_day_end = -1;
    CHECK(SpawnFilter(at, nullptr).filter(day, none) == Why::TimeOfDay || SpawnFilter(at, nullptr).filter(day, none) == Why::Spawns, "a half-open day is judged without a crash");

    content::PopFilter tag = open_filter();
    tag.required_tags = {"npc.stature.rareSpawn"};
    CHECK(f.filter(tag, none) == Why::RequiredTag, "a required tag the spawner lacks");
    CHECK(f.filter(tag, {"npc.stature.rareSpawn"}) == Why::Spawns, "...and one it carries");
    tag.required_tags = {"War.Siege.States.SiegeInactive"};
    CHECK(f.filter(tag, none) == Why::Spawns, "a tag of the world's own (no siege) counts");
}

static content::PopulationData tiny_data() {
    content::PopulationData d;
    content::NpcWorldSpawn ws;
    ws.guid = 1;
    ws.name = "World";
    ws.map_name = "Verra_World_Master";
    d.world_spawns[1] = ws;
    ws.guid = 2;
    ws.name = "Quest";
    ws.map_name = "";
    d.world_spawns[2] = ws;
    ws.guid = 3;
    ws.name = "Dev";
    ws.map_name = "Verra_World_Master";
    ws.dev_only = true;
    d.world_spawns[3] = ws;
    return d;
}
// -- CoA . C_Y_B --

static void test_points() {
    std::printf("the point's rules\n");
    content::PopulationData d = tiny_data();
    content::NpcSetRow set;
    set.guid = 100;
    content::NpcInstanceRow inst;
    inst.guid = 200;
    SpawnContext ctx;
    SpawnFilter f(ctx, nullptr);
    CHECK(f.point(d, &d.world_spawns[1], set, inst) == Why::Spawns, "a world-map point spawns");
    CHECK(f.point(d, &d.world_spawns[2], set, inst) == Why::NotWorldMap, "a quest's spawn (a WorldSpawn with no map) does not");
    CHECK(f.point(d, &d.world_spawns[3], set, inst) == Why::DevOnly, "a dev-only WorldSpawn does not");
    CHECK(f.point(d, nullptr, set, inst) == Why::Spawns, "a placement whose WorldSpawn is no row is judged by the rest");
    content::NpcSetRow dev = set;
    dev.dev_only = true;
    content::NpcInstanceRow dev_i = inst;
    dev_i.dev_only = true;
    CHECK(f.point(d, &d.world_spawns[1], dev, inst) == Why::DevOnly && f.point(d, &d.world_spawns[1], set, dev_i) == Why::DevOnly, "a dev-only set or instance does not");
    content::NpcSetRow black = set;
    black.blacklisted = true;
    CHECK(f.point(d, &d.world_spawns[1], black, inst) == Why::Blacklisted, "a blacklisted set does not");
    d.gates[100] = {"EventTriggerDef", "NarrativeObjective"};
    CHECK(f.point(d, &d.world_spawns[1], set, inst) == Why::Gated, "a set a quest and an event spawn waits for them");
    SpawnContext one = ctx;
    one.active_gates = {"EventTriggerDef"};
    CHECK(SpawnFilter(one, nullptr).point(d, &d.world_spawns[1], set, inst) == Why::Gated, "...both of them");
    one.active_gates = {"EventTriggerDef", "NarrativeObjective"};
    CHECK(SpawnFilter(one, nullptr).point(d, &d.world_spawns[1], set, inst) == Why::Spawns, "...and spawns when both run");
    SpawnContext off = ctx;
    off.enabled = false;
    CHECK(SpawnFilter(off, nullptr).point(d, &d.world_spawns[2], set, inst) == Why::Spawns && SpawnFilter(off, nullptr).point(d, &d.world_spawns[1], black, inst) == Why::Blacklisted,
          "with the filter off a quest's point spawns and a blacklist still holds (stage 2's rule)");
    CHECK(SpawnFilter(off, nullptr).point(d, &d.world_spawns[3], set, inst) == Why::DevOnly, "...and dev-only records never do");
}

static const char* kHeader = "archetype\tparents\tdefault_scale\tstarting_alignments\thalf_height\tradius\tmax_walk_speed\tmax_run_speed\tmax_acceleration\trotation_rate_yaw\tmove_mode\tfly_speed\ttags";
static std::string row13(const std::string& path) {
    return path + "\tNPCPawn_C\t1\t\t88\t34\t600\t600\t2048\t360\tEMovementMode::MOVE_NavWalking\t0\t";
}
static std::string row21(const std::string& path, const std::string& mesh, const std::string& anim, const char* placeholder, const char* hidden) {
    return row13(path) + "\t" + mesh + "\t" + anim + "\t" + placeholder + "\t" + hidden + "\t6064630101073724862;6064631845232640114;6064630135229120908\t6,18,18,6,5,5,10,12\t0:2,22:0\t11,22";
}

static content::NpcAssetRow asset_at(const std::string& leaf) {
    content::NpcAssetRow a;
    a.guid = 1;
    a.name = leaf;
    a.archetype_path = "/Game/DesignAssets/X/" + leaf + ".Default__" + leaf + "_C";
    return a;
}

static void test_visuals_and_table() {
    std::printf("the Blueprint table and the visuals\n");
    npc::MotionTable t;
    std::string e;
    const std::string text = std::string(kHeader) + "\n" + row13("/Game/DesignAssets/X/Old.Default__Old_C") + "\n" + row21("/Game/DesignAssets/X/Wolf.Default__Wolf_C", "SkeletalMesh'SK_NPC_Wolf_A_0'", "AnimBP'Wolf'", "0", "0") + "\n" +
                            row21("/Game/DesignAssets/X/Wail.Default__Wail_C", "SkeletalMesh'SK_PET_Thrall'", "", "1", "0") + "\n" +
                            row21("/Game/DesignAssets/X/Gate.Default__Gate_C", "SkeletalMesh'SK_BeamTarget'", "AnimBP'Bear'", "0", "1") + "\n" +
                            row21("/Game/DesignAssets/X/Pink.Default__Pink_C", "SkeletalMesh'SK_Thing'", "AnimBP'Thing'", "1", "0") + "\n" +
                            row21("/Game/DesignAssets/X/Ghost.Default__Ghost_C", "", "", "0", "0") + "\n" +
                            row21("/Game/DesignAssets/X/Tpose.Default__Tpose_C", "SkeletalMesh'SK_Thing'", "", "0", "0") + "\n" +
                            row21("/Game/DesignAssets/X/Villager.Default__Villager_C", "SkeletalMesh'SK_PC_MASTER'", "AnimBP'NPC'", "0", "0") + "\n" +
                            row21("/Game/DesignAssets/X/Guard_Soldier.Default__Guard_Soldier_C", "SkeletalMesh'SK_PC_MASTER'", "AnimBP'NPC'", "0", "0") + "\n";
    CHECK(t.parse(text, e) && t.size() == 9, "a table of 13- and 21-column lines parses");
    const auto old = t.find("/Game/DesignAssets/X/Old.Default__Old_C");
    CHECK(old && !old->has_visuals_columns && !old->has_combat_columns, "a 13-column line has no visuals and no combat columns");
    const auto wolf = t.find("/Game/DesignAssets/X/Wolf.Default__Wolf_C");
    CHECK(wolf && wolf->has_visuals_columns && wolf->has_combat_columns && wolf->mesh == "SkeletalMesh'SK_NPC_Wolf_A_0'" && wolf->anim == "AnimBP'Wolf'" && !wolf->placeholder && !wolf->hidden,
          "a 21-column line: the mesh and animation");
    CHECK(wolf && wolf->base_list == 6064630101073724862LL && wolf->list1 == 6064631845232640114LL && wolf->list2 == 6064630135229120908LL && wolf->dist[0] == 6 && wolf->dist[1] == 18 &&
              wolf->dist[6] == 10 && wolf->dist[7] == 12 && wolf->elite.size() == 2 && wolf->elite[1] == std::make_pair(22, 0) && wolf->rewards.size() == 2 && wolf->rewards[1] == 22,
          "...the stat lists, the weights, the elite ranks and the reward tables");
    CHECK(!t.parse(std::string(kHeader) + "\n" + row21("/a/b.Default__b_C", "m", "a", "0", "0") + "\textra\n", e), "a 22-column line is refused");
    std::string bad = row21("/a/b.Default__b_C", "m", "a", "0", "0");
    const size_t at = bad.find("6,18,18");
    bad.replace(at, 7, "6,18");
    CHECK(!t.parse(std::string(kHeader) + "\n" + bad + "\n", e), "a stat distribution of fewer than eight weights is refused");

    SpawnContext ctx;
    SpawnFilter f(ctx, &t);
    CHECK(f.visuals(asset_at("Wolf")) == Why::Spawns, "a creature with a mesh and an animation draws");
    CHECK(f.visuals(asset_at("Wail")) == Why::Placeholder, "a mesh with the pink placeholder material does not: the T-posed \"Lingering Wail\"");
    CHECK(f.visuals(asset_at("Gate")) == Why::Hidden, "a hidden Blueprint is a controller or a gate");
    CHECK(f.visuals(asset_at("Pink")) == Why::Placeholder, "a placeholder material alone is enough");
    CHECK(f.visuals(asset_at("Ghost")) == Why::NoMesh, "no mesh");
    CHECK(f.visuals(asset_at("Tpose")) == Why::NoAnimation, "a mesh with no AnimClass stands in a T-pose");
    CHECK(f.visuals(asset_at("Villager")) == Why::Appearance, "a humanoid (the master mesh) needs an NPCAppearance a generic open lacks");
    CHECK(f.visuals(asset_at("Guard_Soldier")) == Why::Spawns, "...but Guard_Soldier, which the stage-2 run showed, draws");
    SpawnContext humans = ctx;
    humans.humanoids = true;
    CHECK(SpawnFilter(humans, &t).visuals(asset_at("Villager")) == Why::Spawns, "--spawn-humanoids lets the humanoids through");
    CHECK(f.visuals(asset_at("Old")) == Why::Spawns && f.visuals(asset_at("NotInTheTable")) == Why::Spawns, "a 13-column line and a Blueprint with no line are no visual verdict (the generic template says not-a-pawn)");
    CHECK(SpawnFilter(ctx, nullptr).visuals(asset_at("Wail")) == Why::Spawns, "with no table nothing is known of the visuals");
    CHECK(spawns::archetype_leaf("/Game/X/Wolf.Default__Wolf_C") == "Wolf" && spawns::archetype_leaf("Wolf_C") == "Wolf" && spawns::archetype_leaf("Wolf") == "Wolf", "archetype_leaf");

    // asset(): the override replaces the asset's own filter, the tags of every level count, a prop tag is a prop
    content::PopulationData d = tiny_data();
    content::NpcSetRow set;
    set.guid = 100;
    content::NpcInstanceRow inst;
    inst.guid = 200;
    content::NpcAssetRow wolf_asset = asset_at("Wolf");
    wolf_asset.filter.cultures = {"Dunir"};
    CHECK(f.asset(d, &d.world_spawns[1], set, inst, 0, wolf_asset) == Why::Culture, "an asset's own filter counts");
    content::PopFilter over = open_filter();
    d.filters[9] = over;
    CHECK(f.asset(d, &d.world_spawns[1], set, inst, 0, wolf_asset) == Why::Culture, "...an unknown override guid changes nothing");
    inst.filter_override = 9;
    CHECK(f.asset(d, &d.world_spawns[1], set, inst, 0, wolf_asset) == Why::Spawns, "the instance's override replaces it");
    inst.filter_override = 0;
    set.filter_override = 9;
    CHECK(f.asset(d, &d.world_spawns[1], set, inst, 0, wolf_asset) == Why::Spawns, "...so does the set's");
    set.filter_override = 0;
    d.world_spawns[1].filter_override = 9;
    CHECK(f.asset(d, &d.world_spawns[1], set, inst, 0, wolf_asset) == Why::Spawns, "...and the WorldSpawn's");
    d.world_spawns[1].filter_override = 0;
    d.filters[8].cultures = {"Vek"};
    inst.filter_override = 8;
    CHECK(f.asset(d, &d.world_spawns[1], set, inst, 0, wolf_asset) == Why::Culture, "the most specific override wins");
    inst.filter_override = 0;
    content::NpcAssetRow tagged = asset_at("Wolf");
    tagged.filter.required_tags = {"a.set"};
    CHECK(f.asset(d, &d.world_spawns[1], set, inst, 5, tagged) == Why::RequiredTag, "a required tag nobody carries");
    d.asset_set_tags[5] = {"a.set"};
    CHECK(f.asset(d, &d.world_spawns[1], set, inst, 5, tagged) == Why::Spawns, "the asset set's tags count");
    d.asset_set_tags.clear();
    set.tags = {"a.set"};
    CHECK(f.asset(d, &d.world_spawns[1], set, inst, 5, tagged) == Why::Spawns, "...so do the set's");
    set.tags.clear();
    inst.tags = {"a.set"};
    CHECK(f.asset(d, &d.world_spawns[1], set, inst, 5, tagged) == Why::Spawns, "...and the instance's");
    content::NpcAssetRow prop = asset_at("Wolf");
    prop.tags = {"npc.type.prop"};
    CHECK(f.asset(d, &d.world_spawns[1], set, inst, 0, prop) == Why::Prop, "an asset tagged npc.type.prop is a prop");
    content::NpcAssetRow dev = asset_at("Wolf");
    dev.dev_only = true;
    CHECK(f.asset(d, &d.world_spawns[1], set, inst, 0, dev) == Why::DevOnly, "a dev-only asset does not spawn");
    SpawnContext off = ctx;
    off.enabled = false;
    CHECK(SpawnFilter(off, &t).asset(d, &d.world_spawns[1], set, inst, 0, wolf_asset) == Why::Spawns && SpawnFilter(off, &t).asset(d, &d.world_spawns[1], set, inst, 0, asset_at("Wail")) == Why::Spawns,
          "with the filter off an asset's conditions and visuals do not count");
}
// ~ CoA & C Y B ~

/// The answer key: the retail captures' Blueprints against the table's meshes.
static void test_answer_key(const npc::MotionTable& table) {
    std::printf("the retail captures against the Blueprints' meshes\n");
    int in_table = 0, with = 0, without = 0, mismatch = 0;
    for (const auto& ev : tests::kNpcAppearanceEvidence) {
        // a Blueprint's path is in the table by its leaf: look it up by suffix
        const npc::MotionStats* found = nullptr;
        npc::MotionStats copy;
        for (const auto& [path, stats] : table.rows()) {
            if (spawns::archetype_leaf(path) == ev.leaf) {
                copy = stats;
                found = &copy;
                break;
            }
        }
        if (!found || !found->has_visuals_columns) continue;
        ++in_table;
        const bool carries = ev.with_component * 2 > ev.epochs;
        (carries ? with : without)++;
        if (carries != found->appearance_dependent()) {
            ++mismatch;
            std::printf("    mismatch: %s (%d of %d opens carry the component) mesh %s\n", ev.leaf, ev.with_component, ev.epochs, found->mesh.c_str());
        }
    }
    CHECK(in_table >= 150 && with >= 80 && without >= 70, "the captures open 150 Blueprints and more that the table knows, humanoids and creatures");
    CHECK(mismatch == 0, "an NPCAppearanceComponent comes with the player's master mesh and with no other: every one of them agrees");
}

static void test_real_area(const content::ContentDb& db, const spawns::CapturedData& captured, const npc::MotionTable& table) {
    std::printf("the real area\n");
    std::string error;
    const auto data = db.population_data(error);
    if (!data) {
        std::printf("  [FAIL] population_data: %s\n", error.c_str());
        g_fail++;
        return;
    }
    CHECK(data->world_spawns.size() == 1089 && data->filters.size() == 247 && data->gates.size() == 1761, "contentc version 9's tables: 1,089 world spawns, 247 filter overrides, 1,761 gated sets");
    size_t world = 0;
    for (const auto& [guid, w] : data->world_spawns) world += w.map_name == "Verra_World_Master";
    CHECK(world == 543, "543 world spawns are the world map's");
    size_t cc = 0;
    for (const auto& [guid, a] : data->assets) cc += a.filter.node_level_min == "cCount";
    CHECK(cc == 44, "44 assets bound their node level with cCount");

    spawns::AreaConfig area;
    spawns::GenericNpcs generic(captured, &table, spawns::GenericConfig{});
    const spawns::TemplateSource templates{&captured, &generic};

    spawns::PopulationStats stage2;
    const auto before = spawns::select_data_spawns(*data, templates, area, stage2);
    SpawnContext ctx;
    ctx.hour = 12;
    SpawnFilter filter(ctx, &table);
    spawns::PopulationStats st;
    const auto after = spawns::select_data_spawns(*data, templates, area, st, &filter);
    std::printf("    stage 2: %zu NPCs; the filter at noon: %zu NPCs, %zu points dropped\n", before.size(), after.size(), st.filtered_total());
    for (const auto& [why, n] : st.filtered) std::printf("      %zu: %s\n", n, spawns::why_text(why));

    CHECK(before.size() > 2000 && after.size() > 800 && after.size() < before.size() / 2, "the filter keeps well under half of what stage 2 spawned, and a good many");
    const auto count = [](const spawns::PopulationStats& s, Why w) { const auto it = s.filtered.find(w); return it == s.filtered.end() ? size_t(0) : it->second; };
    CHECK(count(st, Why::NotWorldMap) > 1000, "the quest, event, layout and test-map points are the most of what goes (1,000 and more)");
    CHECK(count(st, Why::Appearance) > 300, "the humanoids that need an appearance go (300 points and more)");
    CHECK(count(st, Why::Gated) > 0 && count(st, Why::DevOnly) == 0, "gated sets go; the data holds no dev-only record");
    size_t points = st.spawned + st.filtered_total() + st.no_template + st.no_asset;
    CHECK(points == st.points_in_area + st.area_points, "every point of the area is counted once: spawned, dropped by the filter, or lost to a missing template");
// C O A / C_Y_B

    // what the data's NPCs are
    std::map<std::string, size_t> leaves;
    for (const auto& s : after) leaves[spawns::archetype_leaf(s.asset->archetype_path)]++;
    const auto n_of = [&](const char* leaf) { const auto it = leaves.find(leaf); return it == leaves.end() ? size_t(0) : it->second; };
    CHECK(n_of("Goblin_Bonebinder") > 100 && n_of("Wolf") > 100 && n_of("Guard_Soldier") > 30 && n_of("Skeleton") > 50, "retail's creatures and guards spawn: Goblin_Bonebinder, Wolf, Guard_Soldier, Skeleton");
    CHECK(n_of("Quest_NPC_Master") == 0 && n_of("Node_Villager_M") == 0 && n_of("Node_Villager_F") == 0 && n_of("AmbientVillager_Male") == 0 && n_of("Lingering_Wail") == 0,
          "the stage-2 run's invisible villagers and quest masters and its T-posed Wail do not spawn");
    size_t humanoids = 0, no_visuals = 0, other_maps = 0;
    for (const auto& s : after) {
        const auto m = table.find(s.asset->archetype_path);
        if (m && m->appearance_dependent() && spawns::archetype_leaf(s.asset->archetype_path) != "Guard_Soldier") ++humanoids;
        if (m && (m->hidden || m->placeholder || m->mesh.empty() || m->anim.empty())) ++no_visuals;
        const auto ws = data->world_spawns.find(s.placement);
        if (ws != data->world_spawns.end() && ws->second.map_name != "Verra_World_Master") ++other_maps;
    }
    CHECK(humanoids == 0 && no_visuals == 0, "no spawned Blueprint is a humanoid with no look, hidden, placeholder-wearing, mesh-less or T-posed");
    CHECK(other_maps == 0, "no spawned point belongs to another map's WorldSpawn");

    // the captures' own NPCs in the area: Wolf and Goblin_Bonebinder (the first opens of gen_area_spawns.py) are of types that spawn
    for (const auto& c : captured.spawns()) {
        if (c.open->category != "npc" || !area.contains(c.x, c.y)) continue;
        const std::string leaf = spawns::archetype_leaf(c.open->archetype_path);
        if (leaf == "Goblin_Bonebinder" || leaf == "Wolf") CHECK(n_of(leaf.c_str()) > 0, ("the captured " + leaf + " of the area has the data's twins").c_str());
    }

    // the same selection twice is the same list (the hour is the only clock)
    spawns::PopulationStats again;
    const auto twice = spawns::select_data_spawns(*data, templates, area, again, &filter);
    bool same = twice.size() == after.size();
    for (size_t i = 0; same && i < twice.size(); ++i) same = twice[i].x == after[i].x && twice[i].y == after[i].y && twice[i].asset == after[i].asset && twice[i].level == after[i].level;
    CHECK(same && again.filtered == st.filtered, "the same inputs give the same spawns and the same counts");

    // the kill switch: the filter off gives stage 2's area back
    SpawnContext off;
    off.enabled = false;
    SpawnFilter off_filter(off, &table);
    spawns::PopulationStats os;
    const auto restored = spawns::select_data_spawns(*data, templates, area, os, &off_filter);
    CHECK(restored.size() == before.size() && os.filtered_total() == count(os, Why::Blacklisted), "--no-spawn-filter gives stage 2's NPCs back");

    // a night hour brings the night-only spawns (the wolves of the night instances) and keeps the rest
    SpawnContext night = ctx;
    night.hour = 2;
    SpawnFilter nf(night, &table);
    spawns::PopulationStats ns;
    const auto at_night = spawns::select_data_spawns(*data, templates, area, ns, &nf);
    CHECK(count(ns, Why::TimeOfDay) != count(st, Why::TimeOfDay) && at_night.size() != after.size(), "the time of day matters: at 2 in the morning other points spawn than at noon");

    // the gates: turning every source on spawns the gated sets that survive the rest
    SpawnContext all_on = ctx;
    all_on.active_gates = {"NarrativeObjective", "NarrativeQuest", "EventTriggerDef", "StoryArcPhase", "BuildingLevelSpawn", "Layout", "CannonizationState", "EventObjectiveDef",
                           "EventStageDef", "EventDef", "ActionResponse"};
    SpawnFilter gf(all_on, &table);
    spawns::PopulationStats gs;
    const auto with_gates = spawns::select_data_spawns(*data, templates, area, gs, &gf);
    CHECK(count(gs, Why::Gated) == 0 && with_gates.size() >= after.size(), "with every source running no set is gated");

    // the user's humanoid switch
    SpawnContext humans = ctx;
    humans.humanoids = true;
    SpawnFilter hf(humans, &table);
    spawns::PopulationStats hs;
    const auto with_humans = spawns::select_data_spawns(*data, templates, area, hs, &hf);
    CHECK(count(hs, Why::Appearance) == 0 && with_humans.size() > after.size() + 300, "--spawn-humanoids brings the 300 and more humanoid points back");
}

int main() {
    test_conditions();
    test_points();
    test_visuals_and_table();

    const char* env = std::getenv("AOC_CONTENT_DB");
    const std::string path = env && *env ? env : AREA_CONTENT_DB_PATH;
    std::string error;
    auto db = content::ContentDb::open(path, error);
    if (!db) {
        std::printf("  [FAIL] content.db %s does not open: %s\n", path.c_str(), error.c_str());
        return 2;
    }
    std::printf("content.db %s (contentc version %s)\n", path.c_str(), db->meta().contentc_version.c_str());
    auto captured = spawns::CapturedData::load(error);
    if (!captured) {
        std::printf("  [FAIL] the captured spawns do not load: %s\n", error.c_str());
        return 2;
    }
    const char* env_motion = std::getenv("AOC_NPC_MOTION");
    const std::string motion_path = env_motion && *env_motion ? env_motion : "dist/content/npc_motion.tsv";
    npc::MotionTable table;
    if (!table.load(motion_path, error)) {
        std::printf("  [skip] %s: the real-area tests need npc_motion.tsv (tools/mapc: mapc npc-motion)\n", error.c_str());
    } else {
        test_answer_key(table);
        test_real_area(*db, *captured, table);
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
