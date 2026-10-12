// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_world.cpp
// ============================================================================
#include "session/player_world.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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
    ground_report_ = npc::populate(registry_, *npcs_, motion_, *area_report_);
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
    npc_book_.emplace(*bundle_, *gear_);
    combat_ = std::make_unique<combat::CombatWorld>(registry_, npcs_.get(), nav_.get(), *combat_data_, *ability_catalog(), *npc_book_, motion_, combat_config_);
    start_aftermath();
    spdlog::info("[combat] on: {} abilities in the tables, damage profile of {} elements, in combat for {} s, NPCs fight within their aggro radius (default {} cm) and leash at {} cm",
                 ability_catalog()->abilities(), combat_data_->profile().elements.size(), combat_data_->in_combat_timer(), combat_config_.default_aggro_cm,
                 combat_config_.default_leash_cm);
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
    if (city_nodes_ && roads_.level_roads() != 0 && roads_seen_version_ != city_nodes_->version()) apply_road_levels();
    window_cost_.max_people_ms = std::max(window_cost_.max_people_ms, ms(t));
    t = std::chrono::steady_clock::now();
    if (npcs_) npcs_->tick(now_seconds);
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

size_t PlayerWorld::start_node_people(const spawns::NodePopulations::Config& config_in) {
    if (node_pops_ || !config_in.enabled || !city_nodes_ || !node_wire_ || !node_pop_data_ || !area_report_ || !area_report_->data || !captured_ || !generic_) return 0;
    spawns::NodePopulations::Config config = config_in;
    const std::set<std::string> leaves = captured_->npc_leaves();
    config.shown_humanoids.insert(leaves.begin(), leaves.end());
    spawns::NodePopulations::Hooks hooks;
    hooks.spawned = [this](uint64_t id, const content::NpcInstanceRow* instance, const content::NpcRouteRow* route) {
        if (!npcs_) return;   // no ground: the NPC stands where the data put it
        const world::WorldActor* a = registry_.find(id);
        if (!a) return;
        npc::NpcMotionSetup setup = npc::make_setup(*a, motion_, instance, route, npc::SpeedModel{});
        if (setup.ambient == npc::Ambient::Patrol && setup.route.size() < 2) setup.ambient = npc::Ambient::Wander;
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
        spdlog::info("[loot] {} reward tables of the {} the area's creatures name, the experience decay {}", n, roots.size(), decay != 0 ? "of the Adventuring leveling type" : "(none)");
    }
    aftermath_ = std::make_unique<combat::Aftermath>(registry_, *combat_, rewards_ ? &*rewards_ : nullptr, [this] { return guids_.next(); }, aftermath_config_);
    aftermath_->set_quality_of([this](int64_t item) { return item_quality(item); });
    aftermath_->set_name_of([this](int64_t item) { return item_name(item); });
    aftermath_->set_respawner([this](uint32_t ref, double now) { return respawn_npc(ref, now); });
    aftermath_->set_city_nodes(city_nodes_);
    aftermath_->install();
    if (!aftermath_config_.enabled) {
        spdlog::info("[loot] OFF (--no-loot): a death gives no experience and no loot, and nothing respawns (the corpse still goes)");
    } else {
        spdlog::info("[loot] on: a corpse stays {:.0f} s ({:.0f} s after its loot is taken, {:.0f} s unlooted), a spawn point refills its NPC after the instance's respawn time{}{}",
                     aftermath_config_.corpse_seconds, aftermath_config_.emptied_seconds, aftermath_config_.loot_corpse_seconds, aftermath_config_.respawn ? "" : " (respawn off)",
                     aftermath_config_.loot_chance_mult != 1.0 ? "; every RollEachByPercent loot chance x" + std::to_string(aftermath_config_.loot_chance_mult).substr(0, 5) + " (--loot-chance-mult)" : std::string());
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
    if (s.shrine) s.z = spot_height(nav_.get(), s.x, s.y, s.z);
    return s;
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
    double z = 0;
    if (nav_ && nav_->terrain_height(x, y, &z) && std::isfinite(z)) {
        // inside a city node's bowl the baked terrain is the pit under the town (world/town_floor.h): the town floor is where a player stands
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

std::optional<PlayerWorld::TownGround> PlayerWorld::town_ground_at(double x, double y) const {
    double ground = 0;
    if (!nav_ || !nav_->terrain_height(x, y, &ground) || !std::isfinite(ground)) return std::nullopt;
    const auto floor = town_floor().at(x, y, ground);
    if (!floor) return std::nullopt;
    return TownGround{floor->node, ground + floor->lift};
}

std::optional<PlayerWorld::BowlLift> PlayerWorld::lift_out_of_bowl(double x, double y, double z) const {
    const auto town = town_ground_at(x, y);
    if (!town || !std::isfinite(z)) return std::nullopt;
    const double stands = town->z + kCapsuleHalfHeightCm;   // where a player's origin is on the town floor
    if (stands - z < world::TownFloor::kBowlCm) return std::nullopt;   // (a terrace is not "under the floor": only the pit under the town is)
    return BowlLift{town->node, stands + kBowlSlackCm, stands - z};
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
    npc::NpcMotionSetup setup = npc::make_setup(*a, motion_, inst, route, npc::SpeedModel{});
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
