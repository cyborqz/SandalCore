// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_world.cpp
// ============================================================================
#include "session/player_world.h"

#include "combat/npc_replicator.h"
#include "world/interaction_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace aoc { namespace worldd {

namespace {

/// Whether the stat program of content.db is one worldd can serve players with: what its row says it holds, every class running bare
/// without a fault, and the stats the ability runtime reads and sets in each class's output. Empty when it is.
std::string stat_program_problem(const content::StatProgramRow& row, const stats::StatBundle& bundle, const content::AbilityRules& rules) {
    if (row.version != stats::kBundleVersion) return "its format version is " + std::to_string(row.version) + ", this worldd runs " + std::to_string(stats::kBundleVersion);
    if (row.stats != static_cast<int>(bundle.stats.size()) || row.classes != static_cast<int>(bundle.classes.size())) {
        return "its row says " + std::to_string(row.stats) + " stats and " + std::to_string(row.classes) + " classes, the bundle has " +
               std::to_string(bundle.stats.size()) + " and " + std::to_string(bundle.classes.size());
    }
    // the row's checksum is the header's own: a row and a blob of two different builds (or a hand-edited file) are not one program
    if (row.crc32 != bundle.crc32) return "its row says crc32 " + std::to_string(row.crc32) + ", the bundle's header says " + std::to_string(bundle.crc32);
    const char* mode = bundle.mode == stats::Mode::Float32 ? "float32" : bundle.mode == stats::Mode::Double ? "double" : "single";
    if (row.mode != mode) return "its row says mode " + row.mode + ", the bundle's is " + mode;
    // what the ability runtime needs of every class's stats
    std::set<int64_t> needed = {content::kGuidStamina, content::kGuidMaxStamina};
    for (const int64_t effect : {rules.sprint.effect_guid, rules.dodge.effect_guid, rules.rest.effect_guid}) {
        const content::EffectRule* e = rules.effect(effect);
        if (!e) continue;
        for (const content::StatMod& m : e->stat_mods) {
            needed.insert(m.stat_guid);
            if (m.source_stat_guid != 0) needed.insert(m.source_stat_guid);
        }
    }
    for (int class_enum = 1; class_enum <= 8; ++class_enum) {
        stats::StatInputs in;
        in.class_enum = class_enum;
        stats::StatResult result;
        std::string err;
        if (!stats::run_stats(bundle, in, stats::StatState{}, result, err)) return "class " + std::to_string(class_enum) + " does not run: " + err;
        if (result.faults != 0) return "class " + std::to_string(class_enum) + " faults " + std::to_string(result.faults) + " time(s) bare";
        std::set<int64_t> have;
        for (const stats::StatOut& s : result.stats) have.insert(s.guid);
        for (const int64_t guid : needed) {
            if (!have.count(guid)) return "class " + std::to_string(class_enum) + " has no stat " + std::to_string(guid) + ", which the ability runtime reads or sets";
        }
    }
    return {};
}

}  // namespace

bool PlayerWorld::load_mount_speeds(const std::string& path, std::string& error) {
    if (!mounts_) {
        error = "there are no mounts";
        return false;
    }
    if (!mounts_->load_speeds(path, error)) return false;
    spdlog::info("[content] mount speeds: {} Blueprints from {}", mounts_->speeds_known(), path);
    return true;
}

bool PlayerWorld::load_mount_riders(const std::string& path, std::string& error) {
    if (!mounts_) {
        error = "there are no mounts";
        return false;
    }
    if (!mounts_->load_riders(path, error)) return false;
    spdlog::info("[content] mount riders: {} Blueprints name their rider AnimBP in {}", mounts_->riders_known(), path);
    return true;
}
// Cyb with C | O | A

bool PlayerWorld::load_roads(const std::string& path, std::string& error) {
    world::RoadMap fresh;
    if (!fresh.load(path, error, std::clamp(road_default_level_, 0, world::RoadMap::kLevels - 1))) return false;
    roads_ = std::move(fresh);
    roads_seen_version_ = ~0ull;
    spdlog::info("[content] roads: {} road(s), {} segment(s) from {}{}", roads_.roads(), roads_.segments(), path,
                 roads_.node_level() >= 0 ? " (tiers resolved at node level " + std::to_string(roads_.node_level()) + ")" : std::string());
    if (roads_.level_roads() != 0) {
        // the roads' worth follows each owner zone's node (a zone with no node: the default level)
        apply_road_levels();
        spdlog::info("[roads] {} of the {} roads follow their zone's node level (a zone with no node: level {}); the tiers change when a node's level does", roads_.level_roads(), roads_.roads(),
                     road_default_level_);
    }
    return true;
}

PlayerWorld::PlayerWorld(const content::ContentDb* content, std::string content_problem, bootstrap::GameStateMode mode,
                         uint32_t server_id, protocol::ServerGuidAllocator& guids)
    : content_(content), content_problem_(std::move(content_problem)), mode_(mode), server_id_(server_id), guids_(guids),
      game_state_(guids.next()) {
    if (content_) {
        std::string error;
        rules_ = content_->ability_rules(error);
        if (!rules_) {
            // A content.db whose ability rows this worldd cannot run is as good as none: no login until it is rebuilt.
            content_ = nullptr;
            content_problem_ = error;
        } else {
            catalog_.emplace(*content_);
            // plan B3: the stat program and the items' stat data
            std::string problem;
            if (auto row = content_->stat_program(problem)) {
                stats::StatBundle bundle;
                if (!stats::parse_bundle(row->data, bundle, problem)) {
                    problem = "the stat program in content.db does not verify: " + problem;
                } else if (const std::string why = stat_program_problem(*row, bundle, *rules_); !why.empty()) {
                    problem = "the stat program in content.db is not one this worldd can run: " + why;
                } else {
                    bundle_ = std::move(bundle);
                    gear_.emplace(*content_);
                    problem = gear_->ok() ? std::string() : gear_->error();
                    spdlog::info("[content] stat program: {} bytes, {} stats, {} classes, mode {}", row->data.size(), bundle_->stats.size(),
                                 bundle_->classes.size(), row->mode);
                }
            }
            // plan D1: the Adventuring level curve and the ranks its levels grant
            if (problem.empty()) {
                std::string error;
                leveling_type_ = content_->leveling_type_named("Adventuring", error);
                if (!leveling_type_) {
                    problem = "content.db has no usable Adventuring leveling type: " + error;
                } else if (auto curve = leveling::LevelCurve::from(*leveling_type_, error)) {
                    curve_ = std::move(*curve);
                    if (auto grants = content_->auto_grants(leveling_type_->guid, error)) {
                        for (content::AutoGrant& g : *grants) {
                            if (g.tree_guid == content::kGuidUniversalTree) grants_.push_back(std::move(g));
                        }
                        spdlog::info("[content] Adventuring: {} levels, cap {}; {} rank(s) of the Universal tree granted by levels", curve_->max_level(),
                                     curve_->cap(), grants_.size());
                    } else {
                        problem = "content.db's auto-granted ranks cannot be read: " + error;
                    }
                } else {
                    problem = "content.db's Adventuring level curve is not one this worldd can level with: " + error;
                }
            }
            // plan D1, stage 2: the skill trees
            if (problem.empty()) {
                std::string error;
                if (auto data = content_->talent_data(error)) {
                    if (auto catalog = talents::Catalog::from(std::move(*data), error)) {
                        talents_ = std::move(*catalog);
                        spdlog::info("[content] talents: {} skill trees, {} profession tree(s) with ranks a character has from the start", talents_->tree_count(),
                                     talents_->professions().size());
                    } else {
                        problem = "content.db's skill trees are not what this worldd can run: " + error;
                    }
                } else {
                    problem = "content.db's skill trees cannot be read: " + error;
                }
            }
            // Gathering stage 2: the professions (a content.db without their tables runs without professions, and says so)
            if (problem.empty()) {
                std::string why;
                if (auto data = content_->profession_data(why)) {
                    if (auto book = professions::Book::from(*data, why)) {
                        professions_ = std::move(*book);
                        spdlog::info("[content] professions: {} professions, {} certification levels, {} certification actions", professions_->size(), data->certifications.size(),
                                     data->actions.size());
                    } else {
                        problem = "content.db's professions are not what this worldd can run: " + why;
                    }
                } else if (!why.empty()) {
                    problem = "content.db's professions cannot be read: " + why;
                } else {
                    spdlog::warn("[content] content.db has no profession tables: characters have no professions (rebuild content.db with this checkout's contentc)");
                }
            }
            // plan D2: the ability tables (a content.db without them still runs Sprint, Dodge and Rest)
            if (problem.empty()) {
                std::string why;
                if (auto cat = abilities::Catalog::load(content_->path(), *bundle_, why)) {
                    ability_catalog_ = std::move(*cat);
                    spdlog::info("[content] abilities: {} abilities, {} effects, {} hits, {} expressions (the generic ability runtime runs them)",
                                 ability_catalog_->abilities(), ability_catalog_->effects(), ability_catalog_->hits(), ability_catalog_->exprs());
                } else {
                    spdlog::warn("[content] the ability tables are not usable ({}): only Sprint, Dodge and Rest run; rebuild content.db with this checkout's contentc", why);
                }
            }
            // the mounts milestone: the mount tables and the captured donors (a worldd without them runs without mounts, and the log says why)
            if (problem.empty()) {
                std::string why;
                if (auto book = spawns::MountBook::load(*content_, why)) {
                    mounts_ = std::move(*book);
                    spdlog::info("[content] mounts: {} mount items, {} mount abilities, {} mount effects", mounts_->data().mounts.size(), mounts_->data().abilities.size(),
                                 mounts_->data().effects.size());
                } else {
                    spdlog::warn("[content] the mount tables are not usable ({}): no mount can be summoned", why);
                }
            }
            if (!problem.empty()) {
                ability_catalog_.reset();
                professions_.reset();
                talents_.reset();
                bundle_.reset();
                gear_.reset();
                catalog_.reset();
                leveling_type_.reset();
                curve_.reset();
                grants_.clear();
                content_ = nullptr;
                content_problem_ = problem + " (rebuild content.db with this checkout's contentc: cd services && go run ./cmd/contentc)";
            }
        }
    }
}
// built with CoA and C | Y | B

bool PlayerWorld::load_area(const spawns::AreaConfig& area, std::string& error) {
    if (!content_) {
        error = "content.db is not loaded: " + content_problem_;
        return false;
    }
    if (area_report_) {
        error = "the area is loaded already";
        return false;
    }
    std::string e;
    auto captured = spawns::CapturedData::load(e);
    if (!captured) {
        error = "the captured spawns do not load: " + e;
        return false;
    }
    auto gm = vendor::GmVendors::load(*content_, e);
    if (!gm) {
        error = "the GM vendors do not load: " + e;
        return false;
    }
    captured_ = std::move(*captured);
    gm_ = std::move(*gm);
    {   // quests, slice 0: the NPC conversations; a content.db of an older version has none and the NPCs keep their answer
        std::string de;
        if (auto data = content_->dialogue_data(de)) {
            dialogue_.emplace(std::move(*data));
            spdlog::info("[world] conversations: {} nodes of {} roots, {} quests named by conditions", dialogue_->nodes(), dialogue_->roots(), dialogue_->quests());
        } else {
            spdlog::warn("[world] the NPC conversations do not load, so the NPCs that have one answer as before: {}", de);
        }
    }
    {   // vendors, step 0: the towns' shops; a content.db of an older version has none and the NPCs keep their answer
        std::string ve;
        if (auto data = content_->vendor_data(ve)) {
            shops_.emplace(std::move(*data));
            spdlog::info("[world] shops: {} vendor defs, {} NPC links, {} priced listings; open: {}", shops_->defs(), shops_->links(), shops_->priced_rows(), shops_->open_shops().size());
        } else {
            spdlog::warn("[world] the town shops do not load, so the shopkeepers answer as before: {}", ve);
        }
    }
    load_crafting_data();   // crafting, steps 0 and 1: the station definitions and the recipes
    // Gathering stage 3, "Hunting": the creatures whose asset names a Hunting_<kind> interaction get the hunting interactable (spawns/generic_npc.h); an older content.db has none
    if (generic_config_.hunting && generic_config_.hunting_defs.empty()) {
        std::string fe;
        if (const auto foliage = content_->foliage_data(fe)) {
            generic_config_.hunting_defs = gather::hunting_defs(*foliage);
            generic_config_.hunting_names = gather::hunting_names(*foliage);
        }
    }
    generic_.emplace(*captured_, motion_loaded_ ? &motion_ : nullptr, generic_config_);
    spawns::AreaSources sources;
    sources.db = content_;
    sources.captured = &*captured_;
    sources.gm = &*gm_;
    sources.generic = &*generic_;
    spawn_filter_.emplace(spawn_context_, motion_loaded_ ? &motion_ : nullptr);
    sources.filter = &*spawn_filter_;
    area_ = area;
    auto report = spawns::load_area(registry_, sources, area, [this] { return guids_.next(); }, server_id_, 0.0, e);
    if (!report) {
        error = "the area does not load: " + e;
        return false;
    }
    area_report_ = std::move(*report);
    spdlog::info("[area] {}", area_report_->describe());
    for (const std::string& n : area_report_->notes) spdlog::warn("[area] {}", n);
    log_spawn_filter();
    log_npc_types();
    log_huntables();
    if (area_.region) {
        // the data NPCs by the zone of influence they stand in
        std::string line;
        for (size_t z = 0; z < spawns::region::kZoiCount; ++z) {
            const auto it = area_report_->population.by_zoi.find(spawns::region::kZois[z].guid);
            line += (line.empty() ? "" : ", ") + std::string(spawns::region::kZois[z].name) + " " + std::to_string(it == area_report_->population.by_zoi.end() ? 0 : it->second);
        }
        spdlog::info("[area] data NPCs by zone of influence: {}", line);
    }
    spdlog::info("[area] GM vendors: {} PTR vendor defs, {} item records in their stock; of the {} GM/PTR item records, {} are in a stock and {} are in none",
                 gm_->vendors().size(), gm_->stocked_records(), gm_->gm_item_count(), gm_->gm_items_stocked(), gm_->unstocked().size());
    if (!gm_->mount_error().empty()) spdlog::warn("[area] the GM mounts vendor sells only what its def lists: {}", gm_->mount_error());
    else spdlog::info("[area] the GM mounts vendor sells all {} mount items ({} added to the {} its def lists)", gm_->mounts_total(), gm_->mounts_added(), gm_->mounts_total() - gm_->mounts_added());
    // the legend of the vendors: each NPC's nameplate is its def's label, laid out in two arcs in front of the login spot (the first 12 near, the rest further out)
    {
        std::string legend;
        const auto& defs = gm_->vendors();
        for (size_t i = 0; i < defs.size(); ++i) {
            legend += (i == 0 ? "" : "; ") + defs[i].label + " (" + (i < 12 ? "inner" : "outer") + " arc #" + std::to_string(i < 12 ? i + 1 : i - 11) + ", " + std::to_string(defs[i].items.size()) + " items)";
        }
        spdlog::info("[area] GM vendor legend, left to right along each arc: {}", legend);
    }
    return true;
}
// -- C.O.A . C | Y | B --

namespace {

/// "name count, name count ..." of a map sorted by count, in lines of a bounded length, each prefixed.
void log_counts(const char* prefix, std::vector<std::pair<std::string, size_t>> v) {
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    std::string line;
    for (const auto& [name, n] : v) {
        const std::string item = name + " " + std::to_string(n);
        if (!line.empty() && line.size() + item.size() > 400) {
            spdlog::info("{}{}", prefix, line);
            line.clear();
        }
        line += (line.empty() ? "" : ", ") + item;
    }
    if (!line.empty()) spdlog::info("{}{}", prefix, line);
}

}  // namespace

void PlayerWorld::log_spawn_filter() const {
    const spawns::PopulationStats& pop = area_report_->population;
    const spawns::SpawnContext& ctx = spawn_filter_->context();
    if (!ctx.enabled) {
        spdlog::info("[area] spawn filter: OFF (--no-spawn-filter): every point whose Blueprint opens spawns, as in stage 2");
        return;
    }
    spdlog::info("[area] spawn filter: map {}, node level {}, culture {}, season {}, hour {:.1f}, {} gate source(s) running, humanoids {}: {} points dropped, {} spawned",
                 ctx.map_name, ctx.node_level, ctx.culture, ctx.season, ctx.hour, ctx.active_gates.size(), ctx.humanoids ? "all" : "only Guard_Soldier", pop.filtered_total(), pop.spawned);
    std::vector<std::pair<spawns::Why, size_t>> reasons(pop.filtered.begin(), pop.filtered.end());
    std::sort(reasons.begin(), reasons.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    for (const auto& [why, n] : reasons) {
        spdlog::info("[area] spawn filter: {} points: {}", n, spawns::why_text(why));
        if (const auto it = pop.filtered_by_blueprint.find(why); it != pop.filtered_by_blueprint.end()) {
            log_counts("[area] ...   Blueprints: ", {it->second.begin(), it->second.end()});
        }
    }
}

void PlayerWorld::log_hunting_kinds() {
    // Gathering stage 3, "Hunting": which of the kinds the tables can run (a dry run of each with a level-25 Bard's stats)
    if (!gathering_ || gathering_->hunting().empty() || !stat_bundle() || !stat_context().gear) return;
    const bootstrap::StatContext ctx = stat_context();
    stats::CharacterStats cs(*ctx.bundle, *ctx.gear, 2, 25, ctx.rounding);
    std::string err;
    if (!cs.start({}, err)) {
        spdlog::warn("[hunt] the hunting kinds are not checked: the probe character's stats do not start ({})", err);
        return;
    }
    auto run = cs.evaluation({}, err);
    if (!run) {
        spdlog::warn("[hunt] the hunting kinds are not checked: {}", err);
        return;
    }
    const size_t none = gathering_->classify_hunting(*run);
    std::string names;
    for (const auto& [def, k] : gathering_->hunting()) {
        if (k.gives_nothing) names += (names.empty() ? "" : ", ") + k.name;
    }
    spdlog::info("[hunt] {} hunting kinds: {} give no usable item (their items are RETIRED_ or NOTUSED_, or the table never selects one): {}; their corpses are hunted for the experience only. "
                 "The data's NumberOfSubtablesToSelect of 2 is read as 1 for {} tables with two subtables (a hunt gives a carcass or a trainable, not both)",
                 gathering_->hunting().size(), none, names.empty() ? "none" : names, gathering_->hunting_one_of());
}

void PlayerWorld::log_huntables() const {
    // Gathering stage 3, "Hunting": the creatures of the area whose corpse is hunted with a Bow, by kind
    const std::set<int64_t>& defs = generic_config_.hunting_defs;
    if (!generic_config_.hunting) {
        spdlog::info("[hunt] --no-gathering: no creature is huntable");
        return;
    }
    if (defs.empty()) {
        spdlog::info("[hunt] no huntable creatures: content.db has no hunting interactions (rebuild it with contentc 15 or later)");
        return;
    }
    std::map<int64_t, size_t> by_def;
    size_t total = 0;
    registry_.for_each([&](const world::WorldActor& a) {
        if (a.kind == world::ActorKind::Npc && a.npc.interact_def != 0 && defs.count(a.npc.interact_def)) {
            ++by_def[a.npc.interact_def];
            ++total;
        }
    });
    std::map<int64_t, std::string> names;
    std::string names_error;
    if (const auto foliage = content_ ? content_->foliage_data(names_error) : std::nullopt) {
        for (const int64_t d : defs) {
            const auto it = foliage->interaction_defs.find(d);
            if (it != foliage->interaction_defs.end()) names[d] = it->second.name;
        }
    }
    std::vector<std::pair<std::string, size_t>> counts;
    for (const auto& [def, n] : by_def) counts.emplace_back(names.count(def) ? names[def] : std::to_string(def), n);
    spdlog::info("[hunt] huntable creatures in the area: {} of {} NPCs ({} kinds, eight blocks with the hunting interactable{}); a Bow in Equipment slot 38 hunts a corpse for a carcass or a trainable and Hunter XP",
                 total, registry_.count(world::ActorKind::Npc), by_def.size(), generic_ && !generic_->has_hunting_block() ? "; NOT: the Raven's captured open is missing, so they are plain creatures" : "");
    log_counts("[hunt] by kind: ", counts);
}

void PlayerWorld::log_npc_types() const {
    const spawns::PopulationStats& pop = area_report_->population;
    const spawns::GenericNpcs* g = generic_ ? &*generic_ : nullptr;
    if (g) {
        spdlog::info("[area] generic NPCs ({}): donor {}; {} Blueprint types made, {} NPCs; the exports carry checksum {} (the client does not compare 0); kill switch --npc-exclude: {} name(s)",
                     g->config().enabled ? "on" : "off", g->donor().empty() ? "none" : g->donor(), g->made(), pop.generic_spawned, g->config().export_checksum,
                     g->config().exclude.size());
    }
    std::string lost;
    for (const auto& [why, n] : pop.lost_by_verdict) lost += (lost.empty() ? "" : ", ") + std::string(spawns::verdict_name(why)) + " " + std::to_string(n);
    spdlog::info("[area] points that no NPC could be made for: {} ({})", pop.no_template, lost.empty() ? "none" : lost);
    if (!pop.lost_props.empty()) log_counts("[area] ... of which a Blueprint that is no pawn (a prop): ", {pop.lost_props.begin(), pop.lost_props.end()});
    if (!pop.lost_excluded.empty()) log_counts("[area] ... of which left out by --npc-exclude: ", {pop.lost_excluded.begin(), pop.lost_excluded.end()});
    log_counts("[area] NPCs by type: ", {area_report_->npcs_by_archetype.begin(), area_report_->npcs_by_archetype.end()});
    if (!pop.generic_by_archetype.empty()) log_counts("[area] ... made from a generic Blueprint: ", {pop.generic_by_archetype.begin(), pop.generic_by_archetype.end()});
}

bool PlayerWorld::load_blueprints(const std::string& motion_path, std::string& error) {
    if (area_report_) {
        error = "the area is loaded already";
        return false;
    }
    npc::MotionTable table;
    if (!table.load(motion_path, error)) return false;
    motion_ = std::move(table);
    motion_loaded_ = true;
    spdlog::info("[nav] Blueprint table {}: {} pawn Blueprints", motion_path, motion_.size());
    return true;
}

bool PlayerWorld::load_npc_abilities(const std::string& abilities_path, const std::string& montages_path, const std::string& targeting_path, std::string& error) {
    if (combat_) {
        error = "the fights have started";
        return false;
    }
    combat::NpcAbilityData data;
    if (!data.load(abilities_path, montages_path, targeting_path, error)) return false;
    npc_abilities_ = std::move(data);
    spdlog::info("[combat] creature attack animations: {} Blueprints, {} montages", npc_abilities_.kinds(), npc_abilities_.montages());
    // what the tables let each Blueprint's fight use, and what they leave out and why
    for (const std::string& line : npc_abilities_.coverage_lines()) spdlog::info("[combat]   {}", line);
    return true;
}

bool PlayerWorld::load_ground(const std::string& nav_path, const std::string& motion_path, std::string& error) {
    if (!area_report_) {
        error = "the area is not loaded";
        return false;
    }
    if (npcs_) {
        error = "the ground is loaded already";
        return false;
    }
    std::string e;
    auto nav = nav::AreaNav::load(nav_path, &e);
    if (!nav) {
        error = e;
        return false;
    }
    if (!motion_loaded_) {
        std::string me;
        if (!motion_.load(motion_path, me)) {
            spdlog::warn("[nav] no NPC motion table ({}): every NPC stands one capsule half height of 88 cm above its ground and walks at the NPCPawn defaults (run tools/mapc: mapc build)", me);
        } else {
            motion_loaded_ = true;
            spdlog::info("[nav] NPC motion table {}: {} archetypes", motion_path, motion_.size());
        }
    }
    nav_ = std::move(nav);
    preload_window();
    spdlog::info("[nav] ground {}: {} terrain grids, {} navmesh tiles, {} polygons; centre ({}, {}), radius {} m", nav_path, nav_->terrain_grids(), nav_->tile_count(), nav_->polygon_count(),
                 static_cast<long long>(nav_->center_x()), static_cast<long long>(nav_->center_y()), nav_->radius() / 100.0);
    check_ground_covers_area();
    npcs_ = std::make_unique<npc::NpcWorld>(registry_, nav_.get());
    ground_report_ = npc::populate(registry_, *npcs_, motion_, *area_report_, speeds());
    spdlog::info("[nav] {}", ground_report_->describe());
    for (const std::string& line : ground_report_->describe_types()) spdlog::info("[nav] by type (standing/wandering/patrolling): {}", line);
    start_combat();
    if (gm_garden_) place_gm_garden();
    return true;
}
// ~ C O A :: Cyb ~

void PlayerWorld::check_ground_covers_area() const {
    // G1 stage 1: does the ground's footprint (the chunked file's rectangles, a flat file's circle) hold the area?  The area's points are sampled: the Riverlands' rectangles by
    // their corners and centres, a circle on a grid of 100 m.
    size_t sampled = 0, outside = 0;
    const auto probe = [&](double x, double y) {
        ++sampled;
        if (!nav_->in_region(x, y)) ++outside;
    };
    if (area_.region) {
        for (size_t i = 0; i < spawns::region::kRiverlandsRectCount; ++i) {
            const spawns::region::Rect& r = spawns::region::kRiverlandsRects[i];
            probe((r.x0 + r.x1) * 0.5, (r.y0 + r.y1) * 0.5);
            probe(r.x0 + 1.0, r.y0 + 1.0);
            probe(r.x1 - 1.0, r.y1 - 1.0);
        }
    } else {
        for (double dy = -area_.radius_cm; dy <= area_.radius_cm; dy += 10000.0)
            for (double dx = -area_.radius_cm; dx <= area_.radius_cm; dx += 10000.0)
                if (std::hypot(dx, dy) <= area_.radius_cm) probe(area_.center_x + dx, area_.center_y + dy);
    }
    if (area_.region && !nav_->has_region()) {
        // (review B I6) main's dist/content holds E1's flat file under the old name until a merge copies the Riverlands' files over it: worldd's default is the region now
        spdlog::error("[nav] the ground file is a FLAT file (E1's 1 km circle), but the area is the Riverlands: most NPCs have no ground and stand where the data puts them; copy the Riverlands' "
                      "area_nav.bin, area_geom.bin, roads.tsv and roads_raw.tsv into dist/content (E1's flat ones belong in dist/content/e1/), or run worldd with --area-radius 1000 for E1's circle");
    }
    if (outside == 0) {
        spdlog::info("[nav] the ground's footprint holds the whole area ({} points sampled)", sampled);
    } else {
        spdlog::warn("[nav] the ground's footprint {} does not hold {} of {} sampled points of the area ({}): NPCs there have terrain at most and stand where the data puts them "
                     "(rebuild the ground for this area: tools/mapc build{})",
                     nav_->has_region() ? "(the chunked file's rectangles)" : "(a flat file's circle)", outside, sampled, area_.region ? "the Riverlands" : "a circle around the login spot",
                     area_.region ? "" : " --center/--radius");
    }
}

void PlayerWorld::place_gm_garden() {
    if (garden_report_ || !captured_ || !area_report_) return;
    spawns::GardenGround ground;
    if (nav_) {
        ground = [this](double x, double y, double hint, double* z) {
            double terrain = hint;
            if (!nav_->terrain_height(x, y, &terrain)) return false;
            return nav_->ground_height(x, y, terrain, z, 150, 150);   // (a floor within 1.5 m of the terrain, else the terrain: never on a roof)
        };
    }
    garden_report_ = spawns::place_gm_garden(registry_, spawns::garden_donor(*captured_), area_, [this] { return guids_.next(); }, server_id_, 0.0, ground);
    spdlog::info("[area] {}", garden_report_->describe());
    for (const std::string& n : garden_report_->notes) spdlog::warn("[area] {}", n);
}

bool PlayerWorld::adopt_ground(std::unique_ptr<nav::AreaNav> nav, std::string& error) {
    if (npcs_ || !nav) {
        error = npcs_ ? "the ground is loaded already" : "no ground";
        return false;
    }
    nav_ = std::move(nav);
    preload_window();
    npcs_ = std::make_unique<npc::NpcWorld>(registry_, nav_.get());
    start_combat();
    return true;
}

void PlayerWorld::preload_window() {
    if (!nav_ || !nav_->windowed()) return;
    // The chunks read before the NPCs are stood (the one blocking read, at start: the worker still does it, this waits).  A small area (E1's 1 km circle) is loaded whole when its chunks
    // fit in four fifths of the mesh, so that its NPCs are grounded at once; a big one (the region) loads the login spot's window, and the rest of the NPCs wait for the players' windows.
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::pair<double, double>> points;
    int radius = nav::AreaNav::kLoadRadius;
    const double r = area_.radius_cm;
    if (!area_.region && r > 0 && r <= 200000.0) {
        std::map<std::pair<int, int>, uint32_t> tiles_of;
        for (const nav::NavChunkEntry& c : nav_->file().chunks) tiles_of[{c.cx, c.cy}] = c.tiles;
        std::set<std::pair<int, int>> keys;
        size_t tiles = 0;
        for (double dy = -r; dy <= r + 1; dy += 8000.0)
            for (double dx = -r; dx <= r + 1; dx += 8000.0) {
                if (std::hypot(dx, dy) > r + 6000.0) continue;
                const nav::ChunkKey k = nav_->chunk_at(area_.center_x + dx, area_.center_y + dy);
                if (keys.insert({k.x, k.y}).second) {
                    const auto it = tiles_of.find({k.x, k.y});
                    if (it != tiles_of.end()) tiles += it->second;
                    points.emplace_back(area_.center_x + dx, area_.center_y + dy);
                }
            }
        if (tiles * 5 <= static_cast<size_t>(nav_->file().settings.max_tiles) * 4) radius = 0;   // (four fifths of the mesh at most)
        else points.clear();
    }
    if (points.empty()) points.emplace_back(area_.center_x, area_.center_y);
    const bool ok = nav_->load_around(points, radius);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    spdlog::info("[nav] chunked ground: {} chunks ({} tiles) on disk, the terrain ({} grids) resident; the window keeps the chunks within {} of each player and drops one that is more than {} away (at most {} tiles in the mesh); "
                 "{} tiles in {} chunks are in after {:.0f} ms{}", nav_->chunk_count(), nav_->total_tiles(), nav_->terrain_grids(), nav::AreaNav::kLoadRadius, nav::AreaNav::kKeepRadius,
                 nav_->file().settings.max_tiles, nav_->tile_count(), nav_->window_stats().resident_chunks, ms, ok ? "" : " (NOT all of them: the ground there is incomplete)");
}

void PlayerWorld::drive_window(double now_seconds) {
    // the players' chunks come in, the chunks nobody is near go out; the NPCs of a chunk that came in are stood on its ground
    const auto a = std::chrono::steady_clock::now();
    nav_->set_interest(npcs_->player_points(now_seconds));
    const auto b = std::chrono::steady_clock::now();
    const std::vector<nav::ChunkKey> arrived = nav_->pump(kWindowBudgetMs);
    const auto c = std::chrono::steady_clock::now();
    if (!arrived.empty()) {
        npcs_->chunks_resident(arrived);
        window_cost_.grounded_chunks += arrived.size();
        window_cost_.max_ground_ms = std::max(window_cost_.max_ground_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c).count());
    }
    window_cost_.max_interest_ms = std::max(window_cost_.max_interest_ms, std::chrono::duration<double, std::milli>(b - a).count());
    window_cost_.max_pump_ms = std::max(window_cost_.max_pump_ms, std::chrono::duration<double, std::milli>(c - b).count());
    const nav::WindowStats w = nav_->window_stats();
    // every chunk that failed to read or to add is named (the investigation of 2026-10-10: a failure the log did not show could not be told from a window that never moved)
    for (const std::string& e : nav_->take_load_errors()) spdlog::warn("[nav] ground: {}", e);
    if (w.load_failures > window_failures_logged_) {
        spdlog::warn("[nav] {} chunk(s) of the ground could not be loaded or added (damaged or missing file, or no room in the mesh): the NPCs there stay on the terrain", w.load_failures - window_failures_logged_);
        window_failures_logged_ = w.load_failures;
    }
    if (now_seconds - last_window_log_ >= kWindowLogSeconds) {
        last_window_log_ = now_seconds;
        log_window(now_seconds, w);
    }
}

void PlayerWorld::log_window(double now_seconds, const nav::WindowStats& w) {
    // where each noted player stands in the ground's terms, and how much of the NPCs still wait for their chunk
    std::string where;
    size_t players = 0;
    for (const auto& [px, py] : npcs_->player_points(now_seconds)) {
        const nav::ChunkKey k = nav_->chunk_at(px, py);
        const bool here = nav_->ready_at(px, py);
        ++players;
        if (players <= 8) where += (where.empty() ? "" : ", ") + std::string("(") + std::to_string(k.x) + "," + std::to_string(k.y) + (here ? " ready)" : " LOADING)");
    }
    const npc::NpcWorld::Stats ns = npcs_->stats();
    spdlog::info("[nav] window: {} chunks ({} of {} tiles) resident, {} committed, {} players{}{}; NPCs waiting for their chunk {} of {}, {} awake, {} walking; chunks loaded {} / dropped {} / failed {} / retried {}, "
                 "{} deferred for room, {} dropped for a nearer one; planned {} times, {} skipped",
                 w.resident_chunks, w.resident_tiles, nav_->file().settings.max_tiles, w.committed_tiles, players, players ? " in chunks " : "", where, npcs_->pending_ground(), ns.npcs, ns.active_now,
                 ns.moving_now, w.chunk_loads, w.chunk_unloads, w.load_failures, w.retries, w.deferred_by_cap, w.evicted_for_room, w.plans, w.plans_skipped);
}

void PlayerWorld::load_crafting_data() {
    if (!content_ || stations_) return;
    // crafting, step 0: the station definitions; a content.db of an older version has none and the stations stand with the default tax
    std::string se;
    if (auto data = content_->craft_station_data(se)) {
        stations_.emplace(std::move(*data));
        spdlog::info("[world] stations: {} definitions, {} assets that are a station", stations_->defs.size(), stations_->asset_defs.size());
    } else {
        spdlog::warn("[world] the station definitions do not load, so the stations open with the default tax: {}", se);
    }
    // crafting, steps 1 and 2: the recipes, and what each makes when it is done
    std::string re;
    if (auto data = content_->craft_recipe_data(re)) {
        recipes_.emplace(std::move(*data));
        size_t unread = 0, unknown_xp = 0;
        for (const auto& [guid, reward] : recipes_->rewards) {
            unread += reward.unread != 0 ? 1 : 0;
            unknown_xp += reward.unknown_formula.empty() ? 0 : 1;
        }
        spdlog::info("[world] recipes: {} recipes, {} cost records, {} recipe sets, {} reward tables ({} with a part left out, {} with an experience formula not worked out), experience curves {}", recipes_->recipes.size(),
                     recipes_->costs.size(), recipes_->set_lists.size(), recipes_->rewards.size(), unread, unknown_xp, recipes_->xp_curves.ok() ? "read" : "MISSING: no Adventuring experience for a job");
    } else {
        spdlog::warn("[world] the recipes do not load, so no job can be started at a station: {}", re);
    }
}

void PlayerWorld::finish_station(uint64_t id, const world::CityNodeInfo& node, int64_t plot, int64_t building) {
    describe_station(id, node, plot, building);
    if (!npcs_) return;   // no ground: the station stands where the data put it
    if (const world::WorldActor* placed = registry_.find(id)) {
        npc::NpcMotionSetup post;
        post.ambient = npc::Ambient::Stationary;
        post.foot_offset = 0.0;
        post.home_yaw = placed->yaw_degrees;
        npcs_->add(id, post);
    }
}

void PlayerWorld::describe_station(uint64_t id, const world::CityNodeInfo& node, int64_t plot, int64_t building) {
    registry_.modify(id, world::Change::State, [&](world::WorldActor& w) {
        w.station.node = node.guid;
        w.station.plot = plot;
        w.station.asset = w.npc.asset_guid;
        w.station.building = building;
        if (building == 0 && plot != 0 && node_wire_ && city_nodes_) {
            for (const world::PlotRow& row : node_wire_->plot_rows_of(node.guid, city_nodes_->level_of(node.guid))) {
                if (row.plot == plot) {
                    w.station.building = row.building;
                    break;
                }
            }
        }
        if (stations_) {
            if (const content::CraftStationDef* d = stations_->def_of_asset(w.npc.asset_guid)) {
                w.station.def = d->guid;
                w.station.tax_tag = d->tax_tag;
                w.station.processing = d->processing;
                w.station.tier = d->max_tier >= 0 ? d->max_tier + 1 : 0;
            }
        }
        for (protocol::repl::Subobject& s : w.actor.subobjects) {
            if (!s.obj.schema || std::string(s.obj.schema->name) != "UAoCNodePlotComponent") continue;
            s.obj.state.set(*s.obj.schema, "NodeRecordGuid", protocol::repl::Value::i64(node.guid));
            s.obj.state.set(*s.obj.schema, "PlotRecordGuid", protocol::repl::Value::i64(plot));
            s.obj.state.set(*s.obj.schema, "BuildingRecordGuid", protocol::repl::Value::i64(w.station.building));
        }
    });
}

namespace {

bool same_text(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

/// The plots of a node that are free (no building stands on them, none is being built).
std::vector<const world::PlotRow*> free_plots(const std::vector<world::PlotRow>& rows) {
    std::vector<const world::PlotRow*> out;
    for (const world::PlotRow& r : rows) {
        if (r.state == 0 && !r.pre_construct) out.push_back(&r);
    }
    return out;
}

std::string joined(const std::vector<std::string>& names, size_t limit) {
    std::string out;
    for (size_t i = 0; i < names.size() && i < limit; ++i) out += (out.empty() ? "" : ", ") + names[i];
    if (names.size() > limit) out += ", ...";
    return out;
}

}  // namespace

PlayerWorld::StationResult PlayerWorld::place_station(const std::string& node_name, const std::string& plot_name, const std::string& station_name, const std::string& by) {
    StationResult r;
    const auto fail = [&](StationStatus s, std::string m) {
        r.status = s;
        r.message = std::move(m);
        spdlog::info("[stations] {}: place {} {} {}: refused: {}", by, node_name, plot_name, station_name, r.message);
        return r;
    };
    if (!city_nodes_ || !node_wire_ || !node_pop_data_ || !area_report_ || !area_report_->data || !captured_ || !generic_ || !node_pops_) {
        return fail(StationStatus::Refused, "the towns' people are not running (worldd needs the area, the nodes and their people)");
    }
    if (node_name.empty() || plot_name.empty() || station_name.empty()) return fail(StationStatus::Invalid, "a node, a plot and a station are needed");
    const world::CityNodeInfo* node = city_nodes_->by_name(node_name);
    if (!node) return fail(StationStatus::NotFound, "no city node is named \"" + node_name + "\" (worldctl places lists them)");
    const int level = city_nodes_->level_of(node->guid);
    if (level < 3) return fail(StationStatus::Refused, node->display_name + " is a " + std::string(spawns::node_level_name(level)) + ": only a Village or a Town has free service plots");
    const std::vector<world::PlotRow> rows = node_wire_->plot_rows_of(node->guid, level);
    const world::PlotRow* plot = nullptr;
    for (const world::PlotRow& row : rows) {
        if (same_text(row.name, plot_name)) {
            plot = &row;
            break;
        }
    }
    if (!plot) {
        std::vector<std::string> names;
        for (const world::PlotRow* p : free_plots(rows)) names.push_back(p->name);
        return fail(StationStatus::NotFound, node->display_name + " has no plot named \"" + plot_name + "\" (its free plots: " + joined(names, 20) + ")");
    }
    if (plot->state != 0 || plot->pre_construct) return fail(StationStatus::Refused, plot->name + " of " + node->display_name + " is not free (a building stands on it)");
    if (placed_stations_.count({node->guid, plot->plot})) return fail(StationStatus::Refused, plot->name + " of " + node->display_name + " holds a placed station already (worldctl remove-station takes it off)");
    const spawns::NodeSite site = spawns::node_site_of(*city_nodes_, *node_wire_, *node, level);
    const std::vector<int64_t> buildings = node_wire_->plot_buildings(plot->plot);
    const spawns::TemplateSource templates{&*captured_, &*generic_};
    int64_t building = 0;
    std::string why;
    const auto spawn = spawns::select_station_on_plot(*area_report_->data, templates, *node_pop_data_, site, *plot, buildings, station_name, building, why);
    if (!spawn) {
        return fail(StationStatus::NotFound, why + " (the stations the plot's buildings have: " + joined(spawns::stations_on_plot(*area_report_->data, *node_pop_data_, site, buildings), 40) + ")");
    }
    std::string error;
    auto w = spawns::make_data_actor(spawn->spawn, [this] { return guids_.next(); }, server_id_, world_clock_, nullptr, error);
    if (!w) return fail(StationStatus::Refused, "the station could not be made: " + error);
    w->station.placed = true;
    w->npc.spawn_ref = 0;
    w->npc.ground_by_data = true;
    const uint64_t id = w->id;
    const std::string blueprint = w->station.name;
    if (!registry_.add(std::move(*w))) return fail(StationStatus::Refused, "the registry refused the station's actor");
    finish_station(id, *node, plot->plot, building);
    placed_stations_[{node->guid, plot->plot}] = id;
    placed_seen_version_ = city_nodes_->version();
    const world::WorldActor* made = registry_.find(id);
    r.actor = id;
    r.node = node->display_name;
    r.plot = plot->name;
    r.station = blueprint;
    r.building = node_wire_->building_name(plot->plot, building);
    if (made) {
        r.x = made->x;
        r.y = made->y;
        r.z = made->z;
    }
    spdlog::info("[stations] {}: placed {} (actor {}) on {} of {} at ({:.0f}, {:.0f}, {:.0f}), a station of {}; it stays until removed, until the node drops below a Village, or until worldd stops", by, blueprint, id,
                 plot->name, node->display_name, r.x, r.y, r.z, r.building);
    return r;
}

PlayerWorld::StationResult PlayerWorld::remove_station(const std::string& node_name, const std::string& plot_name, const std::string& by) {
    StationResult r;
    const auto fail = [&](StationStatus s, std::string m) {
        r.status = s;
        r.message = std::move(m);
        spdlog::info("[stations] {}: remove {} {}: refused: {}", by, node_name, plot_name, r.message);
        return r;
    };
    if (!city_nodes_) return fail(StationStatus::Refused, "worldd has no city nodes");
    const world::CityNodeInfo* node = city_nodes_->by_name(node_name);
    if (!node) return fail(StationStatus::NotFound, "no city node is named \"" + node_name + "\"");
    for (auto it = placed_stations_.begin(); it != placed_stations_.end(); ++it) {
        if (it->first.first != node->guid) continue;
        const world::WorldActor* a = registry_.find(it->second);
        std::string plot_record;
        for (const world::PlotRow& row : node_wire_ ? node_wire_->plot_rows_of(node->guid, city_nodes_->level_of(node->guid)) : std::vector<world::PlotRow>{}) {
            if (row.plot == it->first.second) plot_record = row.name;
        }
        if (!same_text(plot_record, plot_name)) continue;
        r.node = node->display_name;
        r.plot = plot_record;
        r.station = a ? a->station.name : std::string();
        r.actor = it->second;
        if (a) {
            r.x = a->x;
            r.y = a->y;
            r.z = a->z;
            r.building = node_wire_ ? node_wire_->building_name(it->first.second, a->station.building) : std::string();
        }
        registry_.remove(it->second);
        placed_stations_.erase(it);
        spdlog::info("[stations] {}: removed {} from {} of {}", by, r.station, plot_record, node->display_name);
        return r;
    }
    return fail(StationStatus::NotFound, "no station was placed on " + plot_name + " of " + node->display_name);
}

void PlayerWorld::reap_placed_stations() {
    placed_seen_version_ = city_nodes_ ? city_nodes_->version() : 0;
    for (auto it = placed_stations_.begin(); it != placed_stations_.end();) {
        const int64_t node = it->first.first, plot = it->first.second;
        const int level = city_nodes_->level_of(node);
        bool keep = level >= 3 && node_wire_;
        if (keep) {
            keep = false;
            for (const world::PlotRow& row : node_wire_->plot_rows_of(node, level)) {
                if (row.plot == plot && row.state == 0 && !row.pre_construct) keep = true;
            }
        }
        if (keep && registry_.find(it->second)) {
            ++it;
            continue;
        }
        const world::WorldActor* a = registry_.find(it->second);
        spdlog::info("[stations] the placed station {} of node {} leaves: the node is a {} now{}", a ? a->station.name : std::string("?"), node, spawns::node_level_name(level),
                     level >= 3 ? " and the plot is not free any more" : "");
        registry_.remove(it->second);
        it = placed_stations_.erase(it);
    }
}

void PlayerWorld::check_towns(double now_seconds) {
    if (!node_pops_ || !npcs_ || !nav_ || now_seconds - last_town_check_ < kTownCheckSeconds) return;
    last_town_check_ = now_seconds;
    for (const auto& [node, entry] : node_pops_->entries()) {
        if (entry.npcs == 0) continue;
        std::vector<uint64_t> people = node_pops_->actors_of(node);
        people.erase(std::remove_if(people.begin(), people.end(), [&](uint64_t id) {
            const world::WorldActor* a = registry_.find(id);
            return a && a->kind == world::ActorKind::Station;   // (the stations are counted apart)
        }), people.end());
        const npc::NpcWorld::StandReport r = npcs_->stand_report(people);
        const size_t ready = r.people - r.waiting;
        TownCheck& c = town_checks_[node];
        if (c.level != entry.level || c.people != r.people || c.ready != ready) {
            c.level = entry.level;
            c.people = r.people;
            c.ready = ready;
            c.changed_at = now_seconds;
            c.logged = false;
        }
        // the numbers are still (a window that is loading grounds the people a few at a time), and this level was not logged a moment ago
        if (c.logged || ready == 0 || now_seconds - c.changed_at < kTownSettleSeconds || (c.level == c.logged_level && now_seconds - c.logged_at < kTownRelogSeconds)) continue;
        c.logged = true;
        c.logged_at = now_seconds;
        c.logged_level = c.level;
        const world::CityNodeInfo* info = city_nodes_ ? city_nodes_->by_guid(node) : nullptr;
        spdlog::info("[towns] {} ({}): {} people, {} on a floor within {:.0f} cm of where the data puts them, {} on a floor up to {:.0f} cm from it, {} with no floor in reach ({} of them with no walkable "
                     "ground within {:.0f} cm either: in a wall, a stall, under a floor or in the air; {} of the {} would walk if they had ground), {} waiting for their ground to load",
                     info ? info->display_name : std::to_string(node), spawns::node_level_name(entry.level), r.people, r.on_floor, npc::NpcWorld::kStandNearCm, r.moved, r.max_moved_cm, r.no_floor,
                     r.blocked, npc::NpcWorld::kBlockedCm, r.stuck, r.no_floor, r.waiting);
    }
}

void PlayerWorld::start_combat() {
    if (!combat_config_.enabled) {
        spdlog::info("[combat] OFF (--no-combat): the NPCs do not fight and take no hits");
        return;
    }
    if (!bundle_ || !gear_ || !ability_catalog() || !npcs_) {
        spdlog::warn("[combat] off: it needs the stat program, the item catalog, the ability tables and the NPCs' ground");
        return;
    }
    std::string e;
    const auto rows = content_->combat_rows(e);
    if (!rows) {
        spdlog::warn("[combat] off: {}", e);
        return;
    }
    combat_data_ = combat::CombatData::build(*rows, *bundle_, e);
    if (!combat_data_) {
        spdlog::warn("[combat] off: {}", e);
        return;
    }
    if (!npc_book_) npc_book_.emplace(*bundle_, *gear_);
    combat_ = std::make_unique<combat::CombatWorld>(registry_, npcs_.get(), nav_.get(), *combat_data_, *ability_catalog(), *npc_book_, motion_, combat_config_);
    combat_->set_ability_data(&npc_abilities_);
    if (!npc_abilities_.empty()) spdlog::info("[combat] {}", combat_->gate_census().text());
    start_aftermath();
    spdlog::info("[combat] on: {} abilities in the tables, damage profile of {} elements, in combat for {} s, NPCs fight within their aggro radius (default {} cm) and leash at {} cm, and their hits do {} x the data's damage to a player",
                 ability_catalog()->abilities(), combat_data_->profile().elements.size(), combat_data_->in_combat_timer(), combat_config_.default_aggro_cm,
                 combat_config_.default_leash_cm, combat_config_.npc_damage_mult);
}
// ~ C | O | A - C | Y | B ~

void PlayerWorld::tick_world(double now_seconds) {
    world_clock_ = now_seconds;
    // (review A I2) the Endpoint ticks the world after every datagram it reads: the window is driven at the world's 20 Hz, not at that rate (the NPC world gates itself at 15 Hz)
    if (nav_ && nav_->windowed() && npcs_ && now_seconds - last_window_drive_ >= kWindowPeriodSeconds) {
        last_window_drive_ = now_seconds;
        drive_window(now_seconds);
    }
    const auto ms = [](std::chrono::steady_clock::time_point a) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count(); };
    auto t = std::chrono::steady_clock::now();
    // (Nodes 1, stage 3b) The towns' people and the roads follow the nodes' levels.  Here, after the ground window (the chunks that arrived have grounded the NPCs that waited for them) and
    // before the NPC world's step: an NPC made now goes into the NPC world at once (grounded if its chunk is resident, else waiting for it, pending, as the area's NPCs do), and an NPC
    // removed now is gone from the step that follows.  The world is one thread: the citynode feed only bumps CityNodes::version().
    if (node_pops_) node_pops_->tick(now_seconds);
    if (!placed_stations_.empty() && city_nodes_ && placed_seen_version_ != city_nodes_->version()) reap_placed_stations();
    if (city_nodes_ && roads_.level_roads() != 0 && roads_seen_version_ != city_nodes_->version()) apply_road_levels();
    window_cost_.max_people_ms = std::max(window_cost_.max_people_ms, ms(t));
    t = std::chrono::steady_clock::now();
    if (npcs_) npcs_->tick(now_seconds);
    check_towns(now_seconds);
    window_cost_.max_npcs_ms = std::max(window_cost_.max_npcs_ms, ms(t));
    t = std::chrono::steady_clock::now();
    if (combat_) combat_->tick(now_seconds);
    window_cost_.max_combat_ms = std::max(window_cost_.max_combat_ms, ms(t));
    t = std::chrono::steady_clock::now();
    if (aftermath_) aftermath_->tick(now_seconds);
    window_cost_.max_aftermath_ms = std::max(window_cost_.max_aftermath_ms, ms(t));
    if (scenery_) scenery_->tick();   // a town whose level changed gets a new full payload (every connection that holds its replicator is sent the update)
}

size_t PlayerWorld::apply_road_levels() {
    if (roads_.level_roads() == 0) return 0;
    const world::CityNodes* nodes = city_nodes_;
    if (nodes) roads_seen_version_ = nodes->version();
    const size_t changed = roads_.apply_levels(
        [nodes](int64_t zoi) -> int {
            if (!nodes) return -1;
            const world::CityZoneInfo* z = nodes->zone_by_guid(zoi);
            return z && z->node_guid != 0 ? nodes->level_of(z->node_guid) : -1;   // a zone with no node (a starting area): the world's default level
        },
        road_default_level_);
    if (changed != 0) spdlog::info("[roads] the nodes' levels changed the tier or width of {} of {} roads", changed, roads_.level_roads());
    return changed;
}

uint64_t PlayerWorld::drop_harvest_pile(double x, double y, double z, const protocol::repl::GuidValue& owner, const std::string& name, std::vector<combat::LootStack> stacks,
                                        std::string& error) {
    error.clear();
    if (aftermath_) return aftermath_->drop_pile(x, y, z, owner, name, std::move(stacks), world_clock_, error);
    if (pile_hook_) return pile_hook_(x, y, z, owner, name, stacks);
    error = "the world has no loot piles (no area, or --no-loot)";
    return 0;
}

bool PlayerWorld::start_gathering(const gather::Config& config, std::string& error) {
    error.clear();
    if (gathering_) return true;
    if (!content_) {
        error = "no content.db";
        return false;
    }
    if (!ability_catalog()) {
        error = "no ability catalog (the formulas of a harvest are its programs)";
        return false;
    }
    auto g = gather::Gathering::make(*content_, registry_, *ability_catalog(), config, error);
    if (!g) return false;
    gathering_ = std::move(g);
    spdlog::info("[gather] on: {} node defs, {} tools, {} reward tables; a gathered node returns after the data's respawn x {}{}", gathering_->defs(), gathering_->tools(), gathering_->tables(),
                 config.board.respawn_mult, config.quality_literal ? "; GetRolledItemQuality() answers the literal quality" : "");
    log_hunting_kinds();
    return true;
}

bool PlayerWorld::start_foliage(const foliage::Config& config, std::string& error) {
    error.clear();
    if (foliage_) return true;
    if (!gathering_) {
        error = "gathering is off";
        return false;
    }
    auto f = foliage::Foliage::make(*content_, *gathering_, registry_, [this] { return mint_guid(); }, config, error);
    if (!f) return false;
    foliage_ = std::move(f);
    spdlog::info("[foliage] on: {}", foliage_->describe());
    return true;
}

size_t PlayerWorld::start_node_people(const spawns::NodePopulations::Config& config_in) {
    if (node_pops_ || !config_in.enabled || !city_nodes_ || !node_wire_ || !node_pop_data_ || !area_report_ || !area_report_->data || !captured_ || !generic_) return 0;
    spawns::NodePopulations::Config config = config_in;
    const std::set<std::string> leaves = captured_->npc_leaves();
    config.shown_humanoids.insert(leaves.begin(), leaves.end());
    spawns::NodePopulations::Hooks hooks;
    hooks.spawned = [this](uint64_t id, const content::NpcInstanceRow* instance, const content::NpcRouteRow* route, const world::CityNodeInfo& node, int64_t plot) {
        // A town person opens with the stats its template carried, and some templates carry none (a guard wizard, a villager), which the client shows as 0 health: every one gets the stat
        // program's numbers for its Blueprint and level (Health at its maximum), with or without a ground, fights or players.
        if (bundle_ && gear_) {
            if (const world::WorldActor* person = registry_.find(id); person && person->kind == world::ActorKind::Npc) {
                if (!npc_book_) npc_book_.emplace(*bundle_, *gear_);
                std::string why;
                if (const auto cs = npc_book_->make(motion_.get(person->npc.archetype), person->npc.level, why)) {
                    combat::write_stats(registry_, id, *cs);
                } else {
                    spdlog::warn("[people] {} ({}): its stats do not run, it keeps its template's: {}", id, spawns::blueprint_leaf(person->npc.archetype), why);
                }
            }
        }
        // vendors, step 0: a shopkeeper can be used and its window names the town it stands in (the node its plot component holds is the one the client asks the taxes of and whose
        // name the window shows); a template recorded in another town names that one.  Step 2: so does a shopkeeper whose conversation opens a switched-on shop.
        if (shops_ && node.guid != 0) {
            const world::WorldActor* keeper = registry_.find(id);
            const auto opens_by_talk = [&](const world::WorldActor& npc) {
                if (!dialogue_ || npc.npc.dialogue_root == 0) return false;
                for (const int64_t def : dialogue_->vendor_defs(npc.npc.dialogue_root)) {
                    if (shops_->is_open(def)) return true;
                }
                return false;
            };
            if (keeper && keeper->kind == world::ActorKind::Npc && (shops_->shop_of(keeper->npc.instance_guid, keeper->npc.asset_guid) != 0 || opens_by_talk(*keeper))) {
                registry_.modify(id, world::Change::State, [&](world::WorldActor& w) {
                    world::allow_inputs(w.actor);   // the shopkeeper's four inputs may be activated, as the GM vendors' are
                    for (protocol::repl::Subobject& s : w.actor.subobjects) {
                        if (s.obj.schema && std::string(s.obj.schema->name) == "UAoCNodePlotComponent") s.obj.state.set(*s.obj.schema, "NodeRecordGuid", protocol::repl::Value::i64(node.guid));
                    }
                });
            }
        }
        // crafting, step 0: a station of the node's buildings names its town, plot and building, and stands on the ground like the people
        if (const world::WorldActor* st = registry_.find(id); st && st->kind == world::ActorKind::Station) {
            finish_station(id, node, plot);
            return;
        }
        if (!npcs_) return;   // no ground: the NPC stands where the data put it
        const world::WorldActor* a = registry_.find(id);
        if (!a) return;
        npc::NpcMotionSetup setup = npc::make_setup(*a, motion_, instance, route, speeds(), nullptr, &node);   // (a route of the node is walked at its record)
        if (setup.ambient == npc::Ambient::Patrol && setup.route.size() < 2) setup.ambient = npc::Ambient::Wander;
        if (plot != 0) npc::keep_to_workshop(&setup, npc::kWorkshopRadiusCm);   // a building's person strolls round its own building
        npcs_->add(id, setup);   // (a chunk of the ground that is not resident yet: the NPC waits for it, as the area's do)
    };
    hooks.despawned = [this](uint64_t id) {
        if (combat_) combat_->forget(id);   // (the NPC world forgets it by itself: the registry's removal listener)
    };
    const spawns::TemplateSource templates{&*captured_, &*generic_};
    node_pops_ = std::make_unique<spawns::NodePopulations>(registry_, *city_nodes_, *node_wire_, *node_pop_data_, area_report_->data, templates, spawn_context_,
                                                           motion_loaded_ ? &motion_ : nullptr, area_, [this] { return guids_.next(); }, server_id_, std::move(hooks), config);
    return node_pops_->start(world_clock_);
}

size_t PlayerWorld::start_scenery(const world::NodeScenery::Config& config) {
    if (scenery_ || !config.enabled || !city_nodes_ || !node_wire_ || !node_layout_data_) return 0;
    scenery_ = std::make_unique<world::NodeScenery>(registry_, *city_nodes_, *node_wire_, *node_layout_data_, [this] { return guids_.next(); }, config);
    return scenery_->start();
}

void PlayerWorld::start_aftermath() {
    // the reward tables the area's NPCs name: their Blueprints' (the motion table), their asset's and instance's (the registry's NpcInfo)
    std::set<int64_t> roots;
    for (const auto& [path, row] : motion_.rows()) roots.insert(row.rewards.begin(), row.rewards.end());
    registry_.for_each([&](const world::WorldActor& a) {
        if (a.kind == world::ActorKind::Npc) roots.insert(a.npc.loot_tables.begin(), a.npc.loot_tables.end());
    });
    if (area_report_) {
        for (const spawns::SpawnRecipe& r : area_report_->recipes) roots.insert(r.info.loot_tables.begin(), r.info.loot_tables.end());
    }
    std::string e;
    std::map<int64_t, content::LootTable> tables;
    if (aftermath_config_.enabled && !roots.empty()) {
        auto read = content_->loot_tables(std::vector<int64_t>(roots.begin(), roots.end()), e);
        if (!read) {
            spdlog::warn("[loot] no reward tables: {} (the kills give no experience and no loot)", e);
        } else {
            tables = std::move(*read);
        }
    }
    int64_t decay = 0;
    if (aftermath_config_.enabled) {
        if (const auto r = content_->xp_rule("adventuring_decay", e)) decay = *r;
    }
    if (!tables.empty()) {
        const size_t n = tables.size();
        rewards_.emplace(*ability_catalog(), std::move(tables), decay);
        // item rarities, step 1: the gear and resource lines of a kill roll their item's quality by the line's variation, as a harvest does (a content.db without the variation tables rolls none)
        size_t formulas = 0;
        std::string variation_error;
        if (auto variations = content_->gather_data(variation_error)) {
            formulas = variations->quality_expr.size();
            rewards_->set_quality_rules(std::move(variations->quality_expr), [this](int64_t item, int quality) { return item_rarity(item, quality); });
        } else if (!variation_error.empty()) {
            spdlog::warn("[loot] no item variations: {} (the kills' items keep their record's quality)", variation_error);
        }
        spdlog::info("[loot] {} reward tables of the {} the area's creatures name, the experience decay {}, {} item variation formulas for the quality of the drops", n, roots.size(),
                     decay != 0 ? "of the Adventuring leveling type" : "(none)", formulas);
    }
    aftermath_ = std::make_unique<combat::Aftermath>(registry_, *combat_, rewards_ ? &*rewards_ : nullptr, [this] { return guids_.next(); }, aftermath_config_);
    aftermath_->set_quality_of([this](int64_t item) { return item_quality(item); });
    aftermath_->set_rarity_of([this](int64_t item, int quality) { return item_rarity(item, quality); });
    aftermath_->set_name_of([this](int64_t item) { return item_name(item); });
    aftermath_->set_respawner([this](uint32_t ref, double now) { return respawn_npc(ref, now); });
    aftermath_->set_city_nodes(city_nodes_);
    // Gathering stage 3, "Hunting": a creature whose interaction is a hunting one is hunted when it dies (no loot pile, a corpse that waits 120 s for the hunter)
    const auto huntable = [this](const world::NpcInfo& i) { return gathering_ && i.interact_def != 0 && gathering_->hunt_kind(i.interact_def) != nullptr; };
    aftermath_->set_huntable(huntable);
    combat_->set_huntable(huntable);
    aftermath_->install();
    if (!aftermath_config_.enabled) {
        spdlog::info("[loot] OFF (--no-loot): a death gives no experience and no loot, and nothing respawns (the corpse still goes)");
    } else {
        char boost[32];
        std::snprintf(boost, sizeof boost, "%g", aftermath_config_.loot_rarity_boost);
        spdlog::info("[loot] on: a corpse stays {:.0f} s ({:.0f} s after its loot is taken, {:.0f} s unlooted), a spawn point refills its NPC after the instance's respawn time{}{}{}",
                     aftermath_config_.corpse_seconds, aftermath_config_.emptied_seconds, aftermath_config_.loot_corpse_seconds, aftermath_config_.respawn ? "" : " (respawn off)",
                     aftermath_config_.loot_chance_mult != 1.0 ? "; every RollEachByPercent loot chance x" + std::to_string(aftermath_config_.loot_chance_mult).substr(0, 5) + " (--loot-chance-mult)" : std::string(),
                     aftermath_config_.loot_rarity_boost != 1.0 ? std::string("; TEST BOOST: every outcome above Common in the rarity rolls of a kill's gear and resources x") + boost + " (--loot-rarity-boost; harvests are never boosted)" : std::string());
    }
}
// built by C Y B with C-O-A

PlayerWorld::RespawnSpot PlayerWorld::respawn_spot(double from_x, double from_y) const {
    RespawnSpot s;
    s.x = area_.center_x;
    s.y = area_.center_y;
    s.z = spawns::kLoginSpotZ;
    s.yaw = spawns::kLoginSpotYaw;
    s.name = "the login spot";
    double best = 1e300;
    if (area_report_) {
        for (const spawns::RespawnPoint& p : area_report_->respawn_points) {
            if (!p.attuneable) continue;
            const double d = std::hypot(p.x - from_x, p.y - from_y);
            if (d < best) {
                best = d;
                s.x = p.x;
                s.y = p.y;
                s.z = p.z;
                s.yaw = p.yaw;
                s.shrine = true;
                s.name = p.name;
            }
        }
    }
    if (s.shrine) s.z = spawn_height(s.x, s.y, s.z);
    return s;
}

double PlayerWorld::spawn_height(double x, double y, double data_z, bool* grounded) const {
    // a shrine inside the footprint of a bowl whose platform is read comes back on the platform's floor (the baked ground there is the pit, 50 m under the town) with the slack of every
    // put-down in a town
    if (const auto bowl = bowl_floor_at(x, y)) {
        if (grounded) *grounded = true;
        return bowl->z + kCapsuleHalfHeightCm + kBowlSlackCm;
    }
    return spot_height(nav_.get(), x, y, data_z, grounded);
}

double PlayerWorld::spot_height(const nav::AreaNav* nav, double x, double y, double data_z, bool* grounded) {
    if (grounded) *grounded = false;
    if (!nav || !nav->ready_at(x, y)) return data_z;
    double ground = 0;
    if (!nav->ground_height(x, y, data_z, &ground)) return data_z;
    if (grounded) *grounded = true;
    return ground + 98.0;
}

world::Places PlayerWorld::places() const {
    world::Places out = world::Places::build(city_nodes_, area_.center_x, area_.center_y, spawns::kLoginSpotZ);
    if (garden_report_ && !garden_report_->nodes.empty()) {
        // the GM garden's centre: the mean of its nodes
        world::Place garden;
        garden.name = garden.display_name = garden.kind = "garden";
        for (const spawns::GardenNode& n : garden_report_->nodes) {
            garden.x += n.x;
            garden.y += n.y;
            garden.z += n.z;
        }
        const double count = static_cast<double>(garden_report_->nodes.size());
        garden.x /= count;
        garden.y /= count;
        garden.z /= count;
        out.add(std::move(garden));
    }
    return out;
}

std::optional<double> PlayerWorld::ground_at(double x, double y) const {
    // inside the footprint of a bowl whose platform is read the floor is the platform's (world/town_platform.h), with or without the baked ground: the baked terrain there is the pit
    if (const auto floor = bowl_floor_at(x, y)) return floor->z + kBowlSlackCm;
    double z = 0;
    if (nav_ && nav_->terrain_height(x, y, &z) && std::isfinite(z)) {
        // inside the bowl of a node whose layout is not read the baked terrain is the pit under the town (world/town_floor.h): the interim model's floor is where a player stands
        if (const auto town = town_ground_at(x, y)) return town->z + kBowlSlackCm;
        return z;
    }
    return std::nullopt;
}

void PlayerWorld::refresh_town_floor() const {
    if (!node_pops_ || !nav_) return;   // (no people or no ground: whatever was set stays)
    const uint64_t stamp = (node_pops_->made() << 32) ^ node_pops_->removed();
    if (stamp == town_floor_stamp_) return;
    town_floor_stamp_ = stamp;
    // each node's people where the data put them (the placement's own height, which the NPC world never moves to the terrain for a node's people) over the baked ground under them
    for (const auto& entry : node_pops_->entries()) {
        // a node whose platform is read has its floor from the platform: the model never answers for it (outside the footprint the baked ground meets the platform at the rim)
        if (platforms_ && platforms_->has_node(entry.first)) {
            town_floor_.set(entry.first, {});
            continue;
        }
        std::vector<world::TownFloor::Person> people;
        for (const uint64_t id : node_pops_->actors_of(entry.first)) {
            const world::WorldActor* a = registry_.find(id);
            if (!a || a->kind != world::ActorKind::Npc) continue;
            world::TownFloor::Person p;
            p.x = a->npc.home_x;
            p.y = a->npc.home_y;
            p.z = a->npc.home_z;
            if (!nav_->terrain_height(p.x, p.y, &p.ground)) continue;
            people.push_back(p);
        }
        town_floor_.set(entry.first, people);
    }
}
// (Cyb) (C O A)

world::TownFloor& PlayerWorld::town_floor() const {
    refresh_town_floor();
    return town_floor_;
}

std::optional<PlayerWorld::TownGround> PlayerWorld::bowl_floor_at(double x, double y) const {
    if (!platforms_) return std::nullopt;
    const auto hit = platforms_->floor_at(x, y);
    if (!hit) return std::nullopt;
    // The client has both surfaces inside the footprint, the platform and the baked landscape, and a player stands on the higher.  The baked one is the pit, 50 m under the platform, almost
    // everywhere; but a hill of the landscape pokes through the town (Joeva has one 115 m over its platform, at about 130 m from the node's place) and there it is the ground.
    double z = hit->z, baked = 0;
    if (nav_ && nav_->terrain_height(x, y, &baked) && std::isfinite(baked) && baked > z) z = baked;
    return TownGround{hit->node, z, true};
}

std::optional<PlayerWorld::TownGround> PlayerWorld::town_ground_at(double x, double y) const {
    if (const auto floor = bowl_floor_at(x, y)) return floor;   // (inside a footprint the baked pit is never the answer)
    double ground = 0;
    if (!nav_ || !nav_->terrain_height(x, y, &ground) || !std::isfinite(ground)) return std::nullopt;
    const auto floor = town_floor().at(x, y, ground);
    if (!floor) return std::nullopt;
    return TownGround{floor->node, ground + floor->lift, false};
}

std::vector<double> PlayerWorld::floors_at(double x, double y, double z_lo, double z_hi) const {
    std::vector<double> out;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z_lo) || !std::isfinite(z_hi) || z_hi < z_lo) return out;
    if (nav_ && nav_->ready_at(x, y)) {
        out = nav_->floors_at(x, y, z_lo, z_hi);
        if (out.empty()) {
            // within the agent's radius of a wall the map has no polygon at the exact place: the nearest polygon's floor within a small radius stands in for it
            nav::Vec3 stand;
            if (nav_->nearest_point({x, y, 0.5 * (z_lo + z_hi)}, kNearWallCm, 0.5 * (z_hi - z_lo), &stand) && std::hypot(stand.x - x, stand.y - y) <= kNearWallCm && stand.z >= z_lo && stand.z <= z_hi)
                out.push_back(stand.z);
        }
    }
    // the platform's street is a floor of the place in a bowl, with or without the map's polygon for it
    if (const auto bowl = bowl_floor_at(x, y); bowl && bowl->z >= z_lo && bowl->z <= z_hi) out.push_back(bowl->z);
    // the map's polygon and the platform's exact street are one floor when they agree to the map's resolution (its cells are 10 cm high): no body stands between two floors 25 cm apart
    std::sort(out.begin(), out.end(), std::greater<double>());
    out.erase(std::unique(out.begin(), out.end(), [](double a, double b) { return a - b < 25.0; }), out.end());
    return out;
}

std::optional<double> PlayerWorld::floor_under(double x, double y, double z, double below, double above) const {
    if (!std::isfinite(z) || !(below >= 0) || !(above >= 0)) return std::nullopt;
    const std::vector<double> floors = floors_at(x, y, z - below, z + above);
    if (floors.empty()) return std::nullopt;
    return floors.front();
}

std::optional<PlayerWorld::TownGround> PlayerWorld::town_ground_at(double x, double y, double z) const {
    const auto street = town_ground_at(x, y);
    if (!street || !street->platform || !std::isfinite(z)) return street;
    // a body over a terrace, the floor of a building or a roof is on that, not on the street under it
    const auto floor = floor_under(x, y, z, 60000.0, kFloorReachCm);
    // (the map's street polygon is within its cell height, 10 cm, of the platform's exact street: that is the street, not a floor over it)
    if (floor && *floor > street->z + 25.0) return TownGround{street->node, *floor, true, true};
    return street;
}

std::optional<PlayerWorld::BowlLift> PlayerWorld::lift_out_of_bowl(double x, double y, double z) const {
    const auto town = town_ground_at(x, y);
    if (!town || !std::isfinite(z)) return std::nullopt;
    const double stands = town->z + kCapsuleHalfHeightCm;   // where a player's origin is on the town floor
    // the platform's floor is exact, so a character more than 25 m under it is in the pit under the town (45 to 55 m down for most of the footprint; the outer ring under 25 m is the
    // natural slope at the foot of the rim, where a character can stand outside the walls); the interim model is off by up to 12 m on a terrace (a terrace is not "under the floor"),
    // so it needs 15 m
    const double under = town->platform ? kPlatformUnderCm : world::TownFloor::kBowlCm;
    if (stands - z < under) return std::nullopt;
    return BowlLift{town->node, stands + kBowlSlackCm, stands - z};
}

bool PlayerWorld::load_town_platforms(const std::string& path, std::string& error) {
    auto set = std::make_unique<world::TownPlatforms>();
    if (!set->load(path, &error)) return false;
    size_t placed = 0;
    for (const world::TownBowl& bowl : world::kTownBowls) {
        const world::CityNodeInfo* node = city_nodes_ ? city_nodes_->by_guid(bowl.node) : nullptr;
        if (!node) {
            spdlog::warn("[floor] {} is not in the node registry: its platform is not placed (the interim town floor model answers there)", bowl.name);
            continue;
        }
        if (!set->place(bowl.node, bowl.layout, {node->x, node->y, node->z, node->yaw})) {
            spdlog::warn("[floor] {}: the file has no layout {}: its platform is not placed", node->display_name, bowl.layout);
            continue;
        }
        ++placed;
        // the one number that tells the frame is right, in the log: the platform's floor at the node's place against the baked pit under it (about 50 m down at the five)
        std::string pit;
        double terrain = 0;
        const auto floor = set->floor_at(node->x, node->y);
        if (floor && nav_ && nav_->terrain_height(node->x, node->y, &terrain)) pit = fmt::format(", the baked pit under it {:.1f} m lower", (floor->z - terrain) / 100.0);
        spdlog::info("[floor] {} (node {}): platform {} at the record ({:.0f}, {:.0f}, {:.0f}) facing {:.2f}; the floor at its place is z {:.1f}{}", node->display_name, bowl.node, bowl.layout, node->x,
                     node->y, node->z, node->yaw, floor ? floor->z : 0.0, pit);
    }
    spdlog::info("[floor] {} platform layout(s) read from {}: {} bowl(s) placed; the footprint is the platform's non-hole part (a rounded square of about 900 m), the floor inside it the "
                 "platform's, never the baked pit; the interim town floor model answers only for nodes whose layout is not read",
                 set->layouts(), path, placed);
    platforms_ = std::move(set);
    return true;
}

std::optional<PlayerWorld::FloorRescue> PlayerWorld::floor_rescue(double x, double y, double z) const {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return std::nullopt;
    const auto name_of = [&](int64_t node) {
        const world::CityNodeInfo* n = city_nodes_ ? city_nodes_->by_guid(node) : nullptr;
        return n ? n->display_name : std::to_string(node);
    };
    if (const auto bowl = bowl_floor_at(x, y)) {
        const FloorRescue back{x, y, bowl->z + kCapsuleHalfHeightCm + kBowlSlackCm, bowl->node, "the floor of " + name_of(bowl->node)};
        if (z < bowl->z - kBelowFloorCm || z < kKillPlaneCm) return back;
        // standing in the pit: its feet on the baked ground that lies more than kPlatformUnderCm under the platform (a pawn the landscape's collision caught 50 m under a town whose floor
        // was not there yet; the platform stands over it now, and nothing else would bring it up).  The caller waits a second: a pawn falling through that height is not on it.
        double baked = 0;
        const auto platform = platforms_ ? platforms_->floor_at(x, y) : std::nullopt;
        if (platform && nav_ && nav_->terrain_height(x, y, &baked) && std::isfinite(baked) && platform->z - baked > kPlatformUnderCm && std::fabs(z - kCapsuleHalfHeightCm - baked) < kPitStandCm) {
            FloorRescue pit = back;
            pit.where = "the floor of " + name_of(bowl->node) + " (it stands on the pit " + std::to_string(static_cast<int>((platform->z - baked) / 100.0)) + " m under it)";
            return pit;
        }
        return std::nullopt;
    }
    if (z >= kKillPlaneCm) return std::nullopt;
    // under the kill plane outside a platform's footprint: the town floor of a bowl whose layout is not read, else the ground under the point, else the login spot
    if (const auto town = town_ground_at(x, y)) return FloorRescue{x, y, town->z + kCapsuleHalfHeightCm + kBowlSlackCm, town->node, "the floor of " + name_of(town->node)};
    double ground = 0;
    if (nav_ && nav_->terrain_height(x, y, &ground) && std::isfinite(ground)) return FloorRescue{x, y, ground + kCapsuleHalfHeightCm + kBowlSlackCm, 0, "the ground"};
    return FloorRescue{area_.center_x, area_.center_y, spawns::kLoginSpotZ, 0, "the login spot"};
}

double PlayerWorld::place_height(const world::Place& place) const {
    if (const auto ground = ground_at(place.x, place.y)) return *ground + kCapsuleHalfHeightCm;
    return place.z + kPlaceLiftCm;
}

int PlayerWorld::item_quality(int64_t item) const {
    const auto it = quality_cache_.find(item);
    if (it != quality_cache_.end()) return it->second;
    int q = 1;
    if (content_) {
        std::string err;
        if (const auto rec = content_->item(item, err)) q = std::max(1, rec->default_quality);
    }
    quality_cache_[item] = q;
    return q;
}

const items::ItemRarityFacts& PlayerWorld::item_rarity_facts(int64_t item) const {
    auto it = rarity_cache_.find(item);
    if (it == rarity_cache_.end()) {
        items::ItemRarityFacts facts = items::default_rarity_facts();
        if (content_) {
            std::string err;
            if (const auto rec = content_->item(item, err)) facts = *rec;   // (its rarity facts: the quality curve and RarityMin..RarityMax)
        }
        it = rarity_cache_.emplace(item, facts).first;
    }
    return it->second;
}

int PlayerWorld::item_rarity(int64_t item, int quality) const { return item_rarity_facts(item).rarity_of(static_cast<double>(quality)); }

std::string PlayerWorld::item_name(int64_t item) const {
    const auto it = name_cache_.find(item);
    if (it != name_cache_.end()) return it->second;
    std::string n;
    if (content_) {
        std::string err;
        if (const auto rec = content_->item(item, err)) n = rec->name;
    }
    name_cache_[item] = n;
    return n;
}
// built with C-Y-B and C O A

npc::SpeedModel PlayerWorld::speeds() const {
    npc::SpeedModel s;
    if (generic_config_.hunting) s.standing_defs = &generic_config_.hunting_defs;   // (empty without --gathering: nothing stands)
    return s;
}

uint64_t PlayerWorld::respawn_npc(uint32_t spawn_ref, double now) {
    // an NPC of a town's people comes back from the node's CURRENT selection (spawns/node_populations.h), or not at all when its point is gone
    if (spawns::NodePopulations::is_node_ref(spawn_ref)) return node_pops_ ? node_pops_->respawn(spawn_ref, now) : 0;
    if (!area_report_ || spawn_ref == 0 || spawn_ref > area_report_->recipes.size() || !npcs_ || !captured_) return 0;
    const spawns::SpawnRecipe& r = area_report_->recipes[spawn_ref - 1];
    if (!r.open) return 0;
    spawns::InstanceParams p = r.params;
    p.seed = (p.seed ^ (0xA24BAED4963EE407ull * ++respawns_made_)) * 0x9E3779B97F4A7C15ull;
    p.world_time = now;
    std::string why;
    auto w = spawns::instantiate(*r.open, [this] { return guids_.next(); }, p, why);
    if (!w) {
        spdlog::warn("[loot] a respawn ({}) was refused: {}", r.info.name, why);
        return 0;
    }
    // the recipe's data (level, name, tables, respawn, home, source, spawn_ref, vendor) over the fresh actor's own GUIDs: its controller, its
    // InteractableComponent and its CollisionCylinder are the new block's (E1 review A1: the first generation's cylinder here was refused by the
    // registry while the corpse still stood, and once it was gone the first hit exported that GUID under the new actor = the NetGuidMismatch)
    const uint64_t controller = w->npc.controller, interact = w->npc.interact_component, cylinder = w->npc.collision_cylinder;
    w->npc = r.info;
    w->npc.controller = controller;
    w->npc.interact_component = interact;
    w->npc.collision_cylinder = cylinder;
    const uint64_t id = w->id;
    const std::string name = w->npc.name;
    if (!registry_.add(std::move(*w))) {
        spdlog::warn("[loot] a respawn ({}, spawn point {}) was refused by the registry: actor {} or one of its components is held by another actor", name, spawn_ref, id);
        return 0;
    }
    // its life: the same instance, the same route
    const world::WorldActor* a = registry_.find(id);
    const content::NpcInstanceRow* inst = nullptr;
    const content::NpcRouteRow* route = nullptr;
    if (a->npc.instance_guid != 0) {
        if (const auto it = area_report_->instances.find(a->npc.instance_guid); it != area_report_->instances.end()) inst = &it->second;
        if (inst && inst->patrol_route != 0) {
            if (const auto rt = area_report_->routes.find(inst->patrol_route); rt != area_report_->routes.end()) route = &rt->second;
        }
    }
    npc::NpcMotionSetup setup = npc::make_setup(*a, motion_, inst, route, speeds());
    if (setup.ambient == npc::Ambient::Patrol && setup.route.size() < 2) setup.ambient = npc::Ambient::Wander;
    npcs_->add(id, setup);
    return id;
}

void PlayerWorld::note_player(uint64_t player_id, double x, double y) {
    if (npcs_) npcs_->note_player(player_id, x, y, world_clock_);
    if (!nav_ || !nav_->has_region() || !std::isfinite(x) || !std::isfinite(y)) return;
    // the region's edge (G1 stage 1): beyond it there is no navmesh and nothing spawns, but the player is not held: no wall, and the move checks do not look at the ground
    // (speed, rise and fall only), so the player stays playable.  One log line per exit.
    const bool outside = !nav_->in_region(x, y);
    bool& was = outside_[player_id];
    if (outside && !was) {
        was = true;
        ++region_exits_;
        // (review A M2) the edge is stair-stepped, so a player walking along it crosses in and out often: the exit is logged at most once every 30 s per player (the rest at debug)
        const auto last = outside_logged_at_.find(player_id);
        const bool loud = last == outside_logged_at_.end() || world_clock_ - last->second >= kRegionLogSeconds;
        if (loud) outside_logged_at_[player_id] = world_clock_;
        exit_loud_[player_id] = loud;
        spdlog::log(loud ? spdlog::level::warn : spdlog::level::debug,
                    "[region] player {} left the Riverlands at ({:.0f}, {:.0f}): no navmesh and no spawns beyond the edge; there is no wall, the move checks go on without a ground (logged once per exit, at most every {:.0f} s)",
                    player_id, x, y, kRegionLogSeconds);
    } else if (!outside && was) {
        was = false;
        spdlog::log(exit_loud_[player_id] ? spdlog::level::info : spdlog::level::debug, "[region] player {} is back inside the Riverlands at ({:.0f}, {:.0f})", player_id, x, y);
    }
}

void PlayerWorld::forget_player(uint64_t player_id) {
    if (npcs_) npcs_->forget_player(player_id);
    outside_.erase(player_id);
    outside_logged_at_.erase(player_id);
    exit_loud_.erase(player_id);
}

bootstrap::SessionValues PlayerWorld::new_session(double world_time) {
    bootstrap::SessionValues s;
    s.server_id = server_id_;
    s.world_time = world_time;
    s.player_id = next_player_id_++;
    s.guids = bootstrap::mint_session_guids(guids_, game_state_);
    return s;
}

}}  // namespace aoc::worldd
