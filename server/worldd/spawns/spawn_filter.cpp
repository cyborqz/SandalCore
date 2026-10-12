// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/spawn_filter.cpp  (see spawn_filter.h)
// ============================================================================
#include "spawns/spawn_filter.h"

#include <algorithm>

namespace aoc::worldd::spawns {

const char* why_name(Why w) {
    switch (w) {
        case Why::Spawns: return "spawns";
        case Why::DevOnly: return "dev only";
        case Why::NotWorldMap: return "not the world map's";
        case Why::Blacklisted: return "blacklisted";
        case Why::Gated: return "spawned by a quest, event, phase or building";
        case Why::NodeLevel: return "node level";
        case Why::Culture: return "culture";
        case Why::NodeOrZone: return "node or zone of influence";
        case Why::NodeType: return "node type";
        case Why::Biome: return "biome";
        case Why::Season: return "season";
        case Why::Weather: return "weather";
        case Why::TimeOfDay: return "time of day";
        case Why::RequiredTag: return "required gameplay tag";
        case Why::Prop: return "a prop";
        case Why::Hidden: return "hidden";
        case Why::Placeholder: return "placeholder material";
        case Why::NoMesh: return "no mesh";
        case Why::NoAnimation: return "no animation (T-pose)";
        case Why::Appearance: return "needs an NPCAppearance";
    }
    return "?";
}

const char* why_text(Why w) {
    switch (w) {
        case Why::Spawns: return "spawns";
        case Why::DevOnly: return "a record on its way is bDevOnly";
        case Why::NotWorldMap: return "its WorldSpawn is not the world map's: a quest, event or phase spawn, a node's layout or a test map";
        case Why::Blacklisted: return "its set or instance is blacklisted";
        case Why::Gated: return "a quest objective, an event, a story arc phase, a building level, a layout or an action response spawns its set when its turn comes";
        case Why::NodeLevel: return "its PopulationFilter wants another node level than this world's";
        case Why::Culture: return "its PopulationFilter wants another culture";
        case Why::NodeOrZone: return "its PopulationFilter names a node, or a zone of influence the point is not in";
        case Why::NodeType: return "its PopulationFilter wants another node type";
        case Why::Biome: return "its PopulationFilter wants another biome";
        case Why::Season: return "its PopulationFilter wants another season";
        case Why::Weather: return "its PopulationFilter wants another weather";
        case Why::TimeOfDay: return "its PopulationFilter wants another hour of the game day";
        case Why::RequiredTag: return "its PopulationFilter requires a gameplay tag the spawner does not carry";
        case Why::Prop: return "the asset is tagged npc.type.prop";
        case Why::Hidden: return "the Blueprint's actor or mesh is hidden: a controller, a gate, a cloud";
        case Why::Placeholder: return "the Blueprint's mesh wears the engine's pink placeholder material";
        case Why::NoMesh: return "the Blueprint has no mesh";
        case Why::NoAnimation: return "the Blueprint's mesh has no AnimClass: it would stand in a T-pose";
        case Why::Appearance: return "a humanoid whose body is merged from an NPCAppearanceComponent that a generic open does not carry: it would be invisible";
    }
    return "?";
}
// (C-O-A + C Y B)

std::string archetype_leaf(const std::string& archetype_path) {
    const size_t dot = archetype_path.rfind('.');
    std::string leaf = archetype_path.substr(dot == std::string::npos ? 0 : dot + 1);
    if (leaf.rfind("Default__", 0) == 0) leaf = leaf.substr(9);
    if (leaf.size() > 2 && leaf.compare(leaf.size() - 2, 2, "_C") == 0) leaf.resize(leaf.size() - 2);
    return leaf;
}

int SpawnFilter::node_level_number(const std::string& name) {
    static const char* const kLevels[] = {"Wilderness", "Crossroads", "Encampment", "Village", "Town", "City", "Metropolis", "cCount"};
    for (int i = 0; i < 8; ++i) {
        if (name == kLevels[i]) return i;
    }
    return -1;
}

namespace {

bool has(const std::vector<std::string>& list, const std::string& v) { return std::find(list.begin(), list.end(), v) != list.end(); }

}  // namespace

Why SpawnFilter::point(const content::PopulationData& data, const content::NpcWorldSpawn* world_spawn, const content::NpcSetRow& set, const content::NpcInstanceRow& instance) const {
    if ((world_spawn && world_spawn->dev_only) || set.dev_only || instance.dev_only) return Why::DevOnly;
    if (!ctx_.enabled) {
        // the filter off keeps stage 2's behaviour: the blacklists and nothing else
        return (set.blacklisted || instance.blacklisted) ? Why::Blacklisted : Why::Spawns;
    }
    if (world_spawn && world_spawn->map_name != ctx_.map_name) return Why::NotWorldMap;
    if (set.blacklisted || instance.blacklisted) return Why::Blacklisted;
    if (const auto g = data.gates.find(set.guid); g != data.gates.end()) {
        for (const std::string& source : g->second) {
            if (ctx_.active_gates.count(source) == 0) return Why::Gated;
        }
    }
    return Why::Spawns;
}

Why SpawnFilter::filter(const content::PopFilter& f, const std::set<std::string>& tags, int64_t zoi) const {
    const int level = node_level_number(ctx_.node_level);
    const int lo = node_level_number(f.node_level_min), hi = node_level_number(f.node_level_max);
    // cCount (7) is no level: a filter that bounds the level with it is the design data's "never" (StartArcSpawn and the like)
    if (lo < 0 || hi < 0 || lo >= 7 || hi >= 7 || level < lo || level > hi) return Why::NodeLevel;
    if (!f.cultures.empty() && !has(f.cultures, ctx_.culture)) return Why::Culture;
    if (!f.node_ids.empty() && std::find(f.node_ids.begin(), f.node_ids.end(), ctx_.node_id) == f.node_ids.end()) return Why::NodeOrZone;
    // a zone of influence: the point's own (G1: the Riverlands' grid); an area with none (E1's circle: zoi 0) matches no zone
    // (Nodes 1, stage 3b: the context's own zone stands in for a point that has none, a node's populations)
    const int64_t zone = zoi != 0 ? zoi : ctx_.zone;
    if (!f.zones.empty() && (zone == 0 || std::find(f.zones.begin(), f.zones.end(), zone) == f.zones.end())) return Why::NodeOrZone;
    if (!f.node_types.empty() && !has(f.node_types, ctx_.node_type)) return Why::NodeType;
    if (!f.biomes.empty() && !has(f.biomes, ctx_.biome)) return Why::Biome;
    if (!f.seasons.empty() && !has(f.seasons, ctx_.season)) return Why::Season;
    if (!f.weather.empty() && !has(f.weather, ctx_.weather)) return Why::Weather;
    if (f.time_of_day_start != -1 || f.time_of_day_end != -1) {
        const double a = f.time_of_day_start, b = f.time_of_day_end, h = ctx_.hour;
        const bool in = a <= b ? (h >= a && h <= b) : (h >= a || h <= b);
        if (!in) return Why::TimeOfDay;
    }
    // the siege states are mutually exclusive: the ones a filter requires are any-of (a node carries SiegeInactive); every other required tag is all-of
    constexpr const char* kSiegeStates = "War.Siege.States.";
    bool siege_required = false, siege_carried = false;
    for (const std::string& t : f.required_tags) {
        const bool carried = tags.count(t) != 0 || ctx_.world_tags.count(t) != 0;
        if (t.rfind(kSiegeStates, 0) == 0) {
            siege_required = true;
            siege_carried = siege_carried || carried;
        } else if (!carried) {
            return Why::RequiredTag;
        }
    }
    if (siege_required && !siege_carried) return Why::RequiredTag;
    return Why::Spawns;
}

Why SpawnFilter::visuals(const content::NpcAssetRow& asset) const {
    if (!motion_ || asset.archetype_path.empty()) return Why::Spawns;
    const auto m = motion_->find(asset.archetype_path);
    if (!m || !m->has_visuals_columns) return Why::Spawns;   // not a pawn: the generic template's verdict says so; an old table says nothing
    if (m->hidden) return Why::Hidden;
    if (m->placeholder) return Why::Placeholder;
    if (m->mesh.empty()) return Why::NoMesh;
    if (m->anim.empty()) return Why::NoAnimation;
    if (m->appearance_dependent() && !ctx_.humanoids && ctx_.shown_humanoids.count(archetype_leaf(asset.archetype_path)) == 0) return Why::Appearance;
    return Why::Spawns;
}

Why SpawnFilter::asset(const content::PopulationData& data, const content::NpcWorldSpawn* world_spawn, const content::NpcSetRow& set, const content::NpcInstanceRow& instance,
                       int64_t asset_set, const content::NpcAssetRow& asset, int64_t zoi) const {
    if (asset.dev_only) return Why::DevOnly;
    if (!ctx_.enabled) return Why::Spawns;
    // the filter that counts: the most specific override, else the asset's own
    int64_t override_guid = instance.filter_override;
    if (override_guid == 0) override_guid = set.filter_override;
    if (override_guid == 0 && world_spawn) override_guid = world_spawn->filter_override;
    const content::PopFilter* f = &asset.filter;
    if (override_guid != 0) {
        if (const auto it = data.filters.find(override_guid); it != data.filters.end()) f = &it->second;
    }
    std::set<std::string> tags(instance.tags.begin(), instance.tags.end());
    tags.insert(set.tags.begin(), set.tags.end());
    tags.insert(asset.tags.begin(), asset.tags.end());
    if (const auto it = data.asset_set_tags.find(asset_set); it != data.asset_set_tags.end()) tags.insert(it->second.begin(), it->second.end());
    if (const Why w = filter(*f, tags, zoi); w != Why::Spawns) return w;
    if (std::find(asset.tags.begin(), asset.tags.end(), "npc.type.prop") != asset.tags.end()) return Why::Prop;
    return visuals(asset);
}

}  // namespace aoc::worldd::spawns
