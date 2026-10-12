// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_world.h
//
//  What every player session of one worldd shares (plan A5): the content
//  database, how much of the GameState is sent, worldd's ServerId and GUID
//  allocator, the world's one GameState GUID, and the PlayerId counter.  The
//  Endpoint hands it to each PlayerSession it starts after NMT_Join.
//
//  The GameState is the world's, not the player's: one GUID for the whole worldd
//  run, minted from the allocator before any player's (so a client's GameState
//  channel always names the same object; each connection opens its own channel
//  for it).  Everything else of a joining player is minted per session in the
//  capture's order (bootstrap::mint_session_guids).
//
//  Plan A6 adds what the movement checks and the saving of positions need: the limits the
//  checks use (--max-move-speed lowers the speed), how often a position is saved (60 s) and
//  the PositionSink the sessions hand positions to (worldd's CharacterSaver; none: nothing
//  is saved).
//
//  Plan B2 adds the item catalog the equip rules ask (content.db's item and item_equip_slot; set_item_catalog replaces it in a
//  test) and a PositionSink that also saves inventories (position_saver.h).
//
//  Plan B3 adds the stat program: content.db's stat_program row is parsed and verified once, here, and every class is run bare as a
//  check (it must run, without faults, and give the stats the ability runtime needs); with the items' stat data
//  (content::ContentGearCatalog) it is what every session computes its character's stats with (stat_context()). A content.db
//  without a usable stat program is not ready: no login until it is rebuilt.
//
//  content.db is optional at startup.  When it did not open, ready() is false and
//  content_problem() says why: worldd still runs, and the Endpoint refuses every
//  login with login_text::kContentMissing (a character cannot be valued without
//  it, spec section 5), logging how to build the file.
// ============================================================================
#pragma once

#include "bootstrap/bootstrap.h"
#include "bootstrap/character_setup.h"
#include "combat/aftermath.h"
#include "combat/combat_world.h"
#include "combat/rewards.h"
#include "content/content_db.h"
#include "items/inventory.h"
#include "nav/area_nav.h"
#include "npc/area_npcs.h"
#include "protocol/server_guid_allocator.h"
#include "session/ability_catalog.h"
#include "session/leveling.h"
#include "session/move_checker.h"
#include "session/position_saver.h"
#include "session/actor_view.h"
#include "session/talents.h"
#include "spawns/area.h"
#include "spawns/area_loader.h"
#include "spawns/captured_spawns.h"
#include "spawns/generic_npc.h"
#include "spawns/gm_garden.h"
#include "spawns/mount_spawn.h"
#include "spawns/node_population.h"
#include "spawns/node_populations.h"
#include "stats/stat_program.h"
#include "vendor/gm_vendor.h"
#include "world/actor_registry.h"
#include "world/city_nodes.h"
#include "world/node_control.h"
#include "world/node_layout_data.h"
#include "world/node_scenery.h"
#include "world/node_wire.h"
#include "world/places.h"
#include "world/roads.h"
#include "world/town_floor.h"
// [C.O.A - Cyb]

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace aoc { namespace worldd {

class GmPort;   // session/gm_travel.h

/// How the sessions check moves and save positions (plan A6).
struct MovementConfig {
    MoveLimits                limits;
    std::chrono::seconds      save_interval{60};   // a changed position is saved this often, and when the session ends
};

class PlayerWorld {
public:
    /// `content` is null when content.db did not open (`content_problem` then says why). `guids` must outlive
    /// this object and mint under `server_id`, the id mesh gave worldd.
    PlayerWorld(const content::ContentDb* content, std::string content_problem, bootstrap::GameStateMode mode,
                uint32_t server_id, protocol::ServerGuidAllocator& guids);

    bool ready() const { return content_ != nullptr; }
    const content::ContentDb* content() const { return content_; }
    const std::string& content_problem() const { return content_problem_; }
    /// Plan A7: the ability runtime's numbers (content.db's ability_runtime rows, read once at startup). Always set when
    /// ready(): a content.db whose rows this worldd cannot run is not ready, and says why in content_problem().
    const content::AbilityRules* ability_rules() const { return rules_ ? &*rules_ : nullptr; }
    /// Plan B2: what the equip rules ask of an item record: content.db's, unless a test replaced it. Null when content.db is not loaded.
    const items::ItemCatalog* item_catalog() const { return catalog_override_ ? catalog_override_ : catalog_ ? &*catalog_ : nullptr; }
    void set_item_catalog(const items::ItemCatalog* catalog) { catalog_override_ = catalog; }
    /// Plan B3: the verified stat program and where the items' stat data comes from; a context with null members when the world is not ready.
    /// `stat_rounding` is how an item's interpolated value is made whole (the research's best fit, half up).
    bootstrap::StatContext stat_context() const {
        return {bundle_ ? &*bundle_ : nullptr, gear_ ? &*gear_ : nullptr, stats::Rounding::HalfUp, curve_ ? &*curve_ : nullptr, curve_ ? &grants_ : nullptr,
                talents_ ? &*talents_ : nullptr, mounts_ ? &mounts_->data() : nullptr};
    }
    /// Plan D1: the Adventuring level curve (content.db's leveling_type and leveling_curve) and the Universal tree's auto-granted nodes, read once
    /// at startup; a content.db without them is not ready. Null when the world is not ready.
    const leveling::LevelCurve*            level_curve() const { return curve_ ? &*curve_ : nullptr; }
    const std::vector<content::AutoGrant>& auto_grants() const { return grants_; }
    const content::LevelingType*           leveling_type() const { return leveling_type_ ? &*leveling_type_ : nullptr; }
    /// Plan D1, stage 2: the skill trees, nodes and ranks (content.db's skill_* tables), read once at startup; a content.db without them is not
    /// ready. Null when the world is not ready.
    const talents::Catalog*                talents() const { return talents_ ? &*talents_ : nullptr; }
    const stats::StatBundle* stat_bundle() const { return bundle_ ? &*bundle_ : nullptr; }
    /// Plan D2: every ability, hit, effect and expression of content.db's ab_* tables, read once at startup: what the generic ability runtime
    /// runs. Null when the file has none (an older fixture) or they did not read: then only Sprint, Dodge and Rest run, and the log says why.
    const abilities::Catalog* ability_catalog() const { return catalog_override_set_ ? ability_catalog_override_ : ability_catalog_ ? &*ability_catalog_ : nullptr; }
    void set_ability_catalog(const abilities::Catalog* catalog) {
        ability_catalog_override_ = catalog;
        catalog_override_set_ = true;
    }
    /// The mounts milestone: the mount tables of content.db and the captured donors (null when the file has none: no mount can be summoned).
    const spawns::MountBook* mounts() const { return mounts_ ? &*mounts_ : nullptr; }
    /// A test's mounts (the real ones come from content.db at construction).
    void set_mounts(spawns::MountBook book) { mounts_ = std::move(book); }
    /// Reads the Blueprints' speeds (dist/content/mount_speeds.tsv): a mount runs at its Blueprint's speed when it is desummoned. False, with `error`, for a missing
    /// or broken file (the mounts then use 600 cm/s times the item's ground percentage).
    bool load_mount_speeds(const std::string& path, std::string& error);
    /// Reads the rider AnimBPs of the mount Blueprints (dist/content/mount_riders.tsv, tools/mapc `mount-speeds` writes it): the mount's interaction data names the AnimBP
    /// the rider's mesh is put on. False, with `error`, for a missing or broken file (each mount then names its kind's captured AnimBP).
    bool load_mount_riders(const std::string& path, std::string& error);
    /// The mounts milestone, stage 3: the roads of the area (dist/content/roads.tsv; no extractor yet): a mount on a road rides faster. An empty map (no file) is no roads.
    const world::RoadMap& roads() const { return roads_; }
    world::RoadMap&       roads() { return roads_; }
    /// Reads the roads file. False, with `error`, for a missing or broken file (the map then holds nothing it did not hold before the call).
    bool load_roads(const std::string& path, std::string& error);
    bootstrap::GameStateMode gamestate_mode() const { return mode_; }
    uint32_t server_id() const { return server_id_; }
    const protocol::emit::FIntrepidNetworkGUID& game_state_guid() const { return game_state_; }
    /// Plan D2, review: a fresh GUID of worldd's allocator (a lingering zone's actor and its subobjects, minted while a session runs).
    protocol::emit::FIntrepidNetworkGUID mint_guid() const { return guids_.next(); }

    /// Plan A6: the movement limits and the save interval of every session started from now on.
    void                  set_movement(const MovementConfig& config) { movement_ = config; }
    const MovementConfig& movement() const { return movement_; }
    /// Where the sessions hand positions to be saved; null (the default) saves nothing. Must outlive the sessions.
    void          set_position_sink(PositionSink* sink) { sink_ = sink; }
    PositionSink* position_sink() const { return sink_; }

    /// The per-connection values of one joining player: ServerId, `world_time` (seconds), the next PlayerId and a
    /// fresh set of GUIDs.
    bootstrap::SessionValues new_session(double world_time);

    // ---- plan E1: the living area ------------------------------------------------------------------------------------------
    /// The world's other actors (NPCs, resource nodes, a player's effects): what every session's ActorView draws from. Empty until load_area().
    world::ActorRegistry&       registry() { return registry_; }
    const world::ActorRegistry& registry() const { return registry_; }
    /// Fills the registry with the area around the login spot (spawns/area_loader.h): content.db's population chain, the retail captures' NPCs and
    /// nodes, and the GM vendors. False, with `error`, when content.db is not loaded or lacks the version-7 tables (the registry stays empty). Once.
    bool load_area(const spawns::AreaConfig& area, std::string& error);
    /// Plan E1, stage 2 fix: the NPCs no capture opened (spawns/generic_npc.h).  Before load_area: the Blueprint table (npc_motion.tsv: false, with
    /// `error`, when it is missing or broken: then only the Blueprints a capture opened spawn, and the report says how many points that loses) and the
    /// generic NPCs' switches (the kill switch by name, or all of them off).
    bool load_blueprints(const std::string& motion_path, std::string& error);
    void set_generic_npcs(spawns::GenericConfig config) { generic_config_ = std::move(config); }
    /// Plan E1, stage 3: what this world is for the conditions a spawn names (spawns/spawn_filter.h); before load_area.  `enabled = false` is the kill switch.
    void set_spawn_context(spawns::SpawnContext context) { spawn_context_ = std::move(context); }
    const spawns::SpawnContext& spawn_context() const { return spawn_context_; }
    const npc::MotionTable& blueprints() const { return motion_; }
    const spawns::GenericNpcs* generic_npcs() const { return generic_ ? &*generic_ : nullptr; }
    const spawns::AreaReport*     area_report() const { return area_report_ ? &*area_report_ : nullptr; }
    const spawns::AreaConfig&     area() const { return area_; }
    const vendor::GmVendors*      gm_vendors() const { return gm_ ? &*gm_ : nullptr; }
    /// G1 stage 1: the GM garden (spawns/gm_garden.h), off by default.  `set_gm_garden(true)` before load_area; the garden is placed once the ground is known
    /// (load_ground), or by place_gm_garden() when there is no ground file.  Once.
    void                          set_gm_garden(bool on) { gm_garden_ = on; }
    bool                          gm_garden_requested() const { return gm_garden_; }
    void                          place_gm_garden();
    const spawns::GardenReport*   garden_report() const { return garden_report_ ? &*garden_report_ : nullptr; }
    /// A test's catalog of GM vendors (load_area makes the real one).
    void                          set_gm_vendors(vendor::GmVendors gm) { gm_ = std::move(gm); }
    // ---- plan E1, stage 2: the ground and the NPCs' movement ---------------------------------------------------------------------
    /// Loads the ground (`nav_path`: area_nav.bin, tools/mapc) and the NPC motion table (`motion_path`: npc_motion.tsv), stands every NPC of the area on
    /// it and starts its life (wandering, patrolling): npc/area_npcs.h.  False, with `error`, when the navmesh does not load (the NPCs then stand
    /// where the loader put them, as in stage 1); a missing or broken motion table only means the Blueprint defaults (the log says so).  Call after
    /// load_area; once.
    bool load_ground(const std::string& nav_path, const std::string& motion_path, std::string& error);
    /// Takes a ground that is already built (a test's, or load_ground's after it read the file): the NPCs' world and the fights start over it. The registry's NPCs
    /// are not placed (populate is load_ground's); a test adds its own to `npcs()`. False, with `error`, when there is one already.
    bool                         adopt_ground(std::unique_ptr<nav::AreaNav> nav, std::string& error);
    const nav::AreaNav*          nav() const { return nav_.get(); }
    npc::NpcWorld*               npcs() { return npcs_.get(); }
    const npc::GroundReport*     ground_report() const { return ground_report_ ? &*ground_report_ : nullptr; }
    /// The world clock the last tick set (seconds): what the aftermath's corpses and respawns run on.
    double                       world_clock() const { return world_clock_; }
    /// Advances the NPCs to `now_seconds` (the endpoint's clock, every tick of worldd).
    void                         tick_world(double now_seconds);
    /// A session says where its player stands (every tick); the NPCs near it are awake.
    void                         note_player(uint64_t player_id, double x, double y);
    void                         forget_player(uint64_t player_id);
    /// G1 stage 1: how many times a player walked out of the region (the ground file's footprint) since the start; each exit is logged once.
    uint64_t                     region_exits() const { return region_exits_; }
    /// True while the player's last noted position is beyond the region's edge.
    bool                         outside_region(uint64_t player_id) const { const auto it = outside_.find(player_id); return it != outside_.end() && it->second; }

    // ---- plan E1, stage 3: the fights ---------------------------------------------------------------------------------------------
    /// The combat world's switches (the kill switch `enabled`, the AI's defaults); before load_ground.
    void set_combat_config(combat::CombatWorldConfig config) { combat_config_ = std::move(config); }
    const combat::CombatWorldConfig& combat_config() const { return combat_config_; }
    /// The fights of the area (combat/combat_world.h): null until load_ground made the NPCs' ground, and when combat is off or its data is not in the file.
    combat::CombatWorld* combat() { return combat_.get(); }
// (C_Y_B + C.O.A)

    /// Plan E1, stage 3, part 2: what follows a death (combat/aftermath.h): the experience, the loot, the corpse's life and the respawn.  The switches
    /// (the kill switch `enabled`, the corpse's and the respawn's numbers); before load_ground.
    void set_aftermath_config(combat::AftermathConfig config) { aftermath_config_ = std::move(config); }
    const combat::AftermathConfig& aftermath_config() const { return aftermath_config_; }
    /// Nodes 1, stage 1: the city node registry (world/city_nodes.h), set by main before load_ground; null in a world with no nodes.  The loot rolls in the zone of
    /// influence where the creature died.  The pointer must outlive this object.
    void set_city_nodes(world::CityNodes* nodes) { city_nodes_ = nodes; }
    world::CityNodes* city_nodes() const { return city_nodes_; }
    /// Nodes 1, stage 2: what the client is told about the nodes (world/node_wire.h; built from content.db and the registry), set by main with the registry; null in a world
    /// with no nodes (a GameStateMode::Tier3 join then lists none).  The pointer must outlive this object.
    void set_node_wire(const world::NodeWire* wire) { node_wire_ = wire; }
    const world::NodeWire* node_wire() const { return node_wire_; }
    /// Nodes 1, stage 2: the client's GM console reaches the node service through this (world/node_control.h); null: the node GM RPCs are logged and say the node service is
    /// unavailable.  The pointer must outlive this object.
    void set_node_control(world::NodeControl* control) { node_control_ = control; }
    world::NodeControl* node_control() const { return node_control_; }
    /// Nodes 1, stage 3a: the towns' scenery.  The items of every town's replicator come from this (world/node_layout_data.h; null: no scenery); the pointer must outlive this object.
    void set_node_layout_data(const world::NodeLayoutData* data) { node_layout_data_ = data; }
    /// Makes the replicator of every node that has something to draw at its level (world/node_scenery.h) in the registry, and has tick_world follow the nodes' levels from then on.
    /// Needs the city nodes, the node wire and the layout data; the number of actors made (0 with `config.enabled` false or without those).  Once.
    size_t start_scenery(const world::NodeScenery::Config& config);
    const world::NodeScenery* scenery() const { return scenery_.get(); }
    // ---- Nodes 1, stage 3b: the towns' people and the roads by node level ---------------------------------------------------------------
    /// The node tables the towns' people are selected from (spawns/node_population.h); null: no town has people.  The pointer must outlive this object.
    void set_node_population_data(const spawns::NodePopulationData* data) { node_pop_data_ = data; }
    /// Makes the people of every node inside the area at its level (spawns/node_populations.h), and has tick_world follow the nodes' levels from then on.  Needs the area, the city nodes, the
    /// node wire and the node population data (call it after load_area and load_ground); the NPCs made (0 with `config.enabled` false or without those).  Once.
    size_t start_node_people(const spawns::NodePopulations::Config& config);
    const spawns::NodePopulations* node_people() const { return node_pops_.get(); }
    /// The node level a road of a zone that has no node of its own (a starting area) is worth what it is at (worldd's --spawn-node-level).
    void set_road_default_level(int level) { road_default_level_ = level; }
    /// Sets every by-level road (the roads_levels.tsv file) to the level its zone's node has now.  load_roads and tick_world call it; the number of roads that changed.
    size_t apply_road_levels();
    /// Null until load_ground made the fights (and when combat is off).
    combat::Aftermath*        aftermath() { return aftermath_.get(); }
    const combat::RewardBook* rewards() const { return rewards_ ? &*rewards_ : nullptr; }
    /// Makes the NPC of a spawn point again (the registry gets a new actor; the NPCs' world its life): the new actor id, 0 when it could not.
    uint64_t respawn_npc(uint32_t spawn_ref, double now);
    /// Where a dead player comes back (research/combat 5): the nearest respawn shrine of the area to `(from_x, from_y)`, else the login spot.  The height is the
    /// ground's under the spot plus the player's capsule's half height (98 cm) when the world has ground, else the spot's own.
    struct RespawnSpot {
        double      x = 0, y = 0, z = 0, yaw = 0;
        bool        shrine = false;
        std::string name;
    };
    RespawnSpot respawn_spot(double from_x, double from_y) const;
    /// Review A I4: the height a player is put at over (x, y) when the data says `data_z`: the ground's under the spot plus the capsule's half height (98 cm) when the ground's
    /// chunk is here, the data's own height when it is not (a chunk that is not here has none of its navmesh floors: grounding on the terrain alone would put a shrine on a bridge, or
    /// a platform, under it).  `grounded` (optional) says which.  A world without ground gives the data's.
    static double spot_height(const nav::AreaNav* nav, double x, double y, double data_z, bool* grounded = nullptr);
    /// The default quality of an item record, cached (the loot's stacks).
    int item_quality(int64_t item) const;
    /// The name of an item record ("Certificate_T1_Glint"), cached; empty when content.db has none (the log's loot lines).
    std::string item_name(int64_t item) const;

    // ---- the GM travel tools (session/gm_travel.h) ---------------------------------------------------------------------------------
    /// The client's own GM teleport RPCs (ServerTeleportXYZ, ServerTeleport, ServerTeleportPlayer) are answered only when this is on (--gm-commands / AOC_GM_COMMANDS=1; off by
    /// default).  The admin API (worldctl) does not depend on it: it is the operator's, behind the service token.
    void set_gm_commands(bool on) { gm_commands_ = on; }
    bool gm_commands() const { return gm_commands_; }
    /// What a session asks of the rest of the world for a GM command that names other characters (ServerTeleportPlayer); null: only the session itself can be moved.  Not owned.
    void   set_gm_port(GmPort* port) { gm_port_ = port; }
    GmPort* gm_port() const { return gm_port_; }
    /// The places a GM can send a player to by name (world/places.h): the city nodes that have a place on the Verra map, and the login spot.
    world::Places places() const;
    /// The ground a player is stood on at (x, y) when the server has ground there (the area's navmesh and terrain); nullopt otherwise.  The landscape's height, except inside a city node's bowl
    /// (below): the town's floor plus kBowlSlackCm.
    std::optional<double> ground_at(double x, double y) const;

    // ---- the five city nodes' bowls (world/town_floor.h) ------------------------------------------------------------------------------
    // The baked terrain under Winstead, Joeva, New Aela, Halcyon and Miraleth is a pit about 50.6 m under the town's floor: a player put by the terrain (a GM teleport, a login, the position
    // saved for a rider in the air) would be 50 m under the town.  The floor is known from the heights the town's people stand at.
    /// A put-down inside a town's bowl lands this far (cm) above the modelled floor: the model is within 1.7 m on retail's anchors, so the player falls the last stretch.
    static constexpr double kBowlSlackCm = 200.0;
    /// The town floors.  Built from the node people's heights the first time it is asked after they changed (world/town_floor.h); a world with no node people leaves the table alone, so a
    /// test fills it.
    world::TownFloor& town_floor() const;
    struct TownGround {
        int64_t node = 0;
        double  z = 0;   // the town's floor: the baked ground plus its lift (where a player's feet are; no slack)
    };
    /// The floor of the town whose bowl holds (x, y); none outside every bowl and where the server has no ground.
    std::optional<TownGround> town_ground_at(double x, double y) const;
    struct BowlLift {
        int64_t node = 0;
        double  z = 0;          // where to put the character (its origin): the town's floor + the capsule's half height + kBowlSlackCm
        double  under_cm = 0;   // how far under the town's floor it was saved
    };
    /// A position saved on the baked ground under a town (the floor of its bowl is 15 m or more over the character): where to put the character instead; none otherwise.
    std::optional<BowlLift> lift_out_of_bowl(double x, double y, double z) const;
    /// A player's capsule is this far from its feet to its origin (cm): where a player stands on the ground, its origin is this much above it.
    static constexpr double kCapsuleHalfHeightCm = 98.0;
    /// A named place stands this far above its recorded z where the server has no ground (a short fall, never a place under the ground).
    static constexpr double kPlaceLiftCm = 150.0;
    /// The height a player sent to `place` is put at: the ground under it plus the capsule's half height where the server has ground, else the place's z plus kPlaceLiftCm.
    double place_height(const world::Place& place) const;

    /// How each session's ActorView runs (the rings, the tick, the opens cap): the defaults are retail's; a test changes them before a session joins.
    void                          set_view_config(const ActorView::Config& config) { view_config_ = config; }
    const ActorView::Config&      view_config() const { return view_config_; }
// built by Cyb with C | O | A

private:
    void log_npc_types() const;
    /// Rebuilds the town floors from the node people when those changed since the last build (town_floor()).
    void refresh_town_floor() const;
    /// G1 stage 1: a chunked ground's window (nav/area_nav.h): the blocking read of the login spot's chunks at start, and the per-tick drive (interest, pump, the NPCs of new chunks).
    void preload_window();
    void check_ground_covers_area() const;
    void drive_window(double now_seconds);
    void log_window(double now_seconds, const nav::WindowStats& w);
public:
    /// G1 stage 1: what driving the ground window cost the world tick (the longest of each part, ms) since the start: the interest set, the pump (the net thread's share of the
    /// loading), and standing the NPCs of the chunks that arrived on their ground.  The load test and the log read it.
    struct WindowCost {
        double max_interest_ms = 0, max_pump_ms = 0, max_ground_ms = 0;
        double max_npcs_ms = 0, max_combat_ms = 0, max_aftermath_ms = 0;   // the rest of tick_world: the NPC world's step, the fights, the corpses and respawns
        double max_people_ms = 0;   // Nodes 1, stage 3b: the towns' people following the nodes' levels
        size_t grounded_chunks = 0;
    };
    const WindowCost& window_cost() const { return window_cost_; }
private:
    WindowCost window_cost_;
    static constexpr double kWindowBudgetMs = 3.0;
    /// The window is driven at the world's 20 Hz, not once per network datagram (review A I2): ticks closer than this are skipped.
    static constexpr double kWindowPeriodSeconds = 0.045;
    /// The window's state goes to the log at info this often (world seconds): the next in-game test can be read from it.
    static constexpr double kWindowLogSeconds = 30.0;
    double last_window_drive_ = -1.0e9;
    double last_window_log_ = -1.0e9;
    void start_combat();
    void start_aftermath();
    void log_spawn_filter() const;

    const content::ContentDb*            content_;
    std::string                          content_problem_;
    std::optional<content::AbilityRules> rules_;
    std::optional<items::ContentItemCatalog> catalog_;
    std::optional<stats::StatBundle>         bundle_;
    std::optional<content::ContentGearCatalog> gear_;
    std::optional<content::LevelingType>     leveling_type_;   // plan D1
    std::optional<leveling::LevelCurve>      curve_;
    std::vector<content::AutoGrant>          grants_;          // the Universal tree's, by level
    std::optional<talents::Catalog>          talents_;         // plan D1, stage 2
    std::optional<abilities::Catalog>        ability_catalog_;  // plan D2
    std::optional<spawns::MountBook>         mounts_;           // the mounts milestone
    world::RoadMap                           roads_;            // the mounts milestone, stage 3
    const abilities::Catalog*                ability_catalog_override_ = nullptr;
    bool                                     catalog_override_set_ = false;
    const items::ItemCatalog*            catalog_override_ = nullptr;
    bootstrap::GameStateMode             mode_;
    uint32_t                             server_id_;
    protocol::ServerGuidAllocator&       guids_;
    protocol::emit::FIntrepidNetworkGUID game_state_;
    int32_t                              next_player_id_ = 1;
    MovementConfig                       movement_;
    PositionSink*                        sink_ = nullptr;
    world::ActorRegistry                 registry_;
    spawns::AreaConfig                   area_;
    std::optional<spawns::CapturedData>  captured_;
    std::optional<vendor::GmVendors>     gm_;
    std::optional<spawns::AreaReport>    area_report_;
    bool                                 gm_garden_ = false;
    std::optional<spawns::GardenReport>  garden_report_;
    ActorView::Config                    view_config_;
    std::unique_ptr<nav::AreaNav>        nav_;
    npc::MotionTable                     motion_;
    bool                                 motion_loaded_ = false;
    spawns::GenericConfig                generic_config_;
    spawns::SpawnContext                 spawn_context_;
    std::optional<spawns::SpawnFilter>   spawn_filter_;
    std::optional<spawns::GenericNpcs>   generic_;
    std::unique_ptr<npc::NpcWorld>       npcs_;
    std::optional<npc::GroundReport>     ground_report_;
    combat::CombatWorldConfig            combat_config_;
    std::optional<combat::NpcStatBook>   npc_book_;
    std::optional<combat::CombatData>    combat_data_;
    std::unique_ptr<combat::CombatWorld> combat_;     // after npcs_ and the ground: it is destroyed first
    combat::AftermathConfig              aftermath_config_;
    std::optional<combat::RewardBook>    rewards_;
    std::unique_ptr<combat::Aftermath>   aftermath_;   // after combat_: it is destroyed first (it holds the death hook's `this`)
    world::CityNodes*                    city_nodes_ = nullptr;
    const world::NodeWire*               node_wire_ = nullptr;
    world::NodeControl*                  node_control_ = nullptr;
    const world::NodeLayoutData*         node_layout_data_ = nullptr;
    std::unique_ptr<world::NodeScenery>  scenery_;
    const spawns::NodePopulationData*    node_pop_data_ = nullptr;
    std::unique_ptr<spawns::NodePopulations> node_pops_;   // after the NPC world, the fights and the aftermath: their hooks point at them
    int                                  road_default_level_ = 0;
    uint64_t                             roads_seen_version_ = ~0ull;
    mutable world::TownFloor             town_floor_;           // the towns in a bowl (town_floor())
    mutable uint64_t                     town_floor_stamp_ = ~0ull;   // the node people it was built from (their made + removed counts)
    bool                                 gm_commands_ = false;
    GmPort*                              gm_port_ = nullptr;
    uint64_t                             respawns_made_ = 0;
    mutable std::map<int64_t, int>       quality_cache_;
    mutable std::map<int64_t, std::string> name_cache_;
    double                               world_clock_ = 0;
    std::unordered_map<uint64_t, bool>   outside_;     // per player: beyond the region's edge at the last note
    std::unordered_map<uint64_t, double> outside_logged_at_;   // per player: the world time of its last logged exit (review A M2: a rate limit; the exits are counted all the same)
    std::unordered_map<uint64_t, bool>   exit_loud_;           // per player: the exit it is outside after was logged (its re-entry is then logged too)
    static constexpr double kRegionLogSeconds = 30.0;
    uint64_t                             region_exits_ = 0;
    uint64_t                             window_failures_logged_ = 0;
};

}}  // namespace aoc::worldd
