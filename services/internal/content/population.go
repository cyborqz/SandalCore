// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

// Plan E1, stage 1 (contentc version 7): the NPC population chain and the vendors; stage 2 (version 8): the routes' waypoints and pauses and the instances'
// patrol settings.
//
// The chain, from a blueprint up to where it stands (research/actors/evidence.md section 7):
//
//	npc_asset              a PopulationAsset: the NPC's blueprint (asset_ref, and archetype_path, the path of its class default object, which the
//	                       client names in a SerializeNewActor), its tags, faction, aggro and leash radius, scale, dialogue root, interaction
//	npc_asset_loot         the RewardTables its LootTablesIds name, in order
//	npc_asset_set,
//	npc_asset_set_member   an AssetSet: weighted assets
//	npc_instance           a PopulationInstance: which AssetSets it draws from (npc_instance_asset_set), its level range, respawn time, rule and
//	                       variance, patrol route, wander and speed settings, vendor table, loot tables (npc_instance_loot)
//	npc_set,
//	npc_set_member         a PopulationSet: instances at local offsets (x, y, z in cm, yaw in degrees), and its PopulationArea (0 none)
//	npc_world_spawn,
//	npc_placement          a WorldSpawn: the sets it places, each at an origin and a yaw (a node's layout is placed at the origin (0, 0) and
//	                       relative to the node)
//	npc_area,
//	npc_area_circle        a PopulationArea: the circles (centre, radius in cm, weight) its instances spawn inside, and how many may
//	npc_route              a PathingRoute: the patrol path as the actor's transform, the spline's points and (version 8) the waypoint locations and
//	                       the pause at each (stage 2 walks it)
//
//	vendor_def, vendor_def_inventory, vendor_inventory, vendor_listing
//	                       a VendorDef with its inventories (the tabs of the vendor window) and each inventory's listings (an item, and the names of
//	                       the offers that price it: an empty list is free)
//
// The NPC stat initializer lists (NPC_Base, the elite scalars, one list per creature) are in stat_init already.
//
// Version 9 (plan E1, stage 3: the spawn filter) adds what decides whether a point spawns for a character now: the PopulationFilter of an asset
// (npc_asset.filter) and the FilteringOverride that may replace it on an instance, a set or a world spawn (npc_filter), bDevOnly, the gameplay tags
// of the instance, the set and the asset set, the spawner priority, and npc_gate: the sets that a quest objective, an event, a story arc phase, a
// building level, a layout or an action response spawns when its turn comes (a set no such record names is the world's own population).

import (
	"database/sql"
	"fmt"
)

// C.O.A captures, C-Y-B code

// PopulationDDL creates the tables above.
const PopulationDDL = `
CREATE TABLE npc_asset (
    guid                    INTEGER PRIMARY KEY,
    name                    TEXT NOT NULL,
    asset_ref               TEXT NOT NULL,
    archetype_path          TEXT NOT NULL,
    faction_guid            INTEGER NOT NULL,
    aggro_radius            REAL NOT NULL,
    leash_radius            REAL NOT NULL,
    override_aggro          INTEGER NOT NULL,
    override_leash          INTEGER NOT NULL,
    scale_min               REAL NOT NULL,
    scale_max               REAL NOT NULL,
    tags                    TEXT NOT NULL,   -- a JSON array of gameplay tag names
    dialogue_root_guid      INTEGER NOT NULL,
    interact_component_guid INTEGER NOT NULL,
    interaction_def_guid    INTEGER NOT NULL,
    appearance_guid         INTEGER NOT NULL,
    dev_only                INTEGER NOT NULL,   -- (contentc version 9) the record's bDevOnly
    filter                  TEXT NOT NULL       -- (contentc version 9) the PopulationFilter as JSON: see PopulationFilterJSON
);
CREATE INDEX npc_asset_by_path ON npc_asset (archetype_path);
CREATE TABLE npc_asset_loot (
    asset_guid INTEGER NOT NULL,
    ord        INTEGER NOT NULL,
    table_guid INTEGER NOT NULL,
    PRIMARY KEY (asset_guid, ord)
) WITHOUT ROWID;
CREATE TABLE npc_asset_set (
    guid INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    tags TEXT NOT NULL                          -- (contentc version 9) the GameplayTags, a JSON array
);
CREATE TABLE npc_asset_set_member (
    set_guid   INTEGER NOT NULL,
    ord        INTEGER NOT NULL,
    asset_guid INTEGER NOT NULL,
    weight     REAL NOT NULL,
    PRIMARY KEY (set_guid, ord)
) WITHOUT ROWID;
CREATE TABLE npc_instance (
    guid                    INTEGER PRIMARY KEY,
    name                    TEXT NOT NULL,
    level_min               INTEGER NOT NULL,
    level_max               INTEGER NOT NULL,
    dynamic_leveling        INTEGER NOT NULL,
    respawn_time            REAL NOT NULL,
    respawn_variance        REAL NOT NULL,
    respawn_rule            TEXT NOT NULL,
    patrol_route_guid       INTEGER NOT NULL,
    ambient_behavior        TEXT NOT NULL,
    min_radius              REAL NOT NULL,
    max_radius              REAL NOT NULL,
    arrival_distance        REAL NOT NULL,
    wander_speed_mult       REAL NOT NULL,
    wander_delay            REAL NOT NULL,
    wander_delay_deviation  REAL NOT NULL,
    speed_expr              TEXT NOT NULL,
    snapping_rule           TEXT NOT NULL,
    orientation_type        TEXT NOT NULL,
    aggro_radius            REAL NOT NULL,
    override_aggro          INTEGER NOT NULL,
    area_weight             REAL NOT NULL,
    vendor_guid             INTEGER NOT NULL,
    interact_component_guid INTEGER NOT NULL,
    interact_response_guid  INTEGER NOT NULL,
    dialogue_guid           INTEGER NOT NULL,
    group_id                INTEGER NOT NULL,
    spawn_location_type     TEXT NOT NULL,
    activation_range        REAL NOT NULL,
    blacklisted             INTEGER NOT NULL,
    num_waypoints           INTEGER NOT NULL,   -- (contentc version 8) how many waypoints of its route a patrol walks, 0: all
    max_angle               REAL NOT NULL,
    follow_distance         REAL NOT NULL,
    formation               TEXT NOT NULL,
    idle_anim               TEXT NOT NULL,
    align_to_slope          INTEGER NOT NULL,
    dev_only                INTEGER NOT NULL,   -- (contentc version 9) bDevOnly
    filter_override_guid    INTEGER NOT NULL,   -- (contentc version 9) FilteringOverrideId (npc_filter), 0 none
    spawner_priority        TEXT NOT NULL,      -- (contentc version 9) Always, High, Medium, Low
    tags                    TEXT NOT NULL       -- (contentc version 9) the GameplayTags, a JSON array
);
CREATE TABLE npc_instance_asset_set (
    instance_guid INTEGER NOT NULL,
    ord           INTEGER NOT NULL,
    set_guid      INTEGER NOT NULL,
    PRIMARY KEY (instance_guid, ord)
) WITHOUT ROWID;
CREATE TABLE npc_instance_loot (
    instance_guid INTEGER NOT NULL,
    ord           INTEGER NOT NULL,
    table_guid    INTEGER NOT NULL,
    PRIMARY KEY (instance_guid, ord)
) WITHOUT ROWID;
CREATE TABLE npc_set (
    guid             INTEGER PRIMARY KEY,
    name             TEXT NOT NULL,
    area_guid        INTEGER NOT NULL,
    activation_range REAL NOT NULL,
    blacklisted      INTEGER NOT NULL,
    dev_only             INTEGER NOT NULL,      -- (contentc version 9) bDevOnly
    filter_override_guid INTEGER NOT NULL,      -- (contentc version 9) FilterOverrideId (npc_filter), 0 none
    tags                 TEXT NOT NULL          -- (contentc version 9) the GameplayTags, a JSON array
);
CREATE TABLE npc_set_member (
    set_guid        INTEGER NOT NULL,
    key             INTEGER NOT NULL,
    instance_guid   INTEGER NOT NULL,
    x               REAL NOT NULL,
    y               REAL NOT NULL,
    z               REAL NOT NULL,
    yaw             REAL NOT NULL,
    random_rotation INTEGER NOT NULL,
    PRIMARY KEY (set_guid, key)
) WITHOUT ROWID;
CREATE TABLE npc_world_spawn (
    guid     INTEGER PRIMARY KEY,
    name     TEXT NOT NULL,
    culture  TEXT NOT NULL,
    map_name TEXT NOT NULL,
    dev_only             INTEGER NOT NULL,      -- (contentc version 9) bDevOnly
    filter_override_guid INTEGER NOT NULL       -- (contentc version 9) FilterOverrideId (npc_filter), 0 none
);
CREATE TABLE npc_placement (
    spawn_guid INTEGER NOT NULL,
    key        INTEGER NOT NULL,
    set_guid   INTEGER NOT NULL,
    x          REAL NOT NULL,
    y          REAL NOT NULL,
    z          REAL NOT NULL,
    yaw        REAL NOT NULL,
    min_level  INTEGER NOT NULL,
    max_level  INTEGER NOT NULL,
    PRIMARY KEY (spawn_guid, key)
) WITHOUT ROWID;
CREATE TABLE npc_area (
    guid             INTEGER PRIMARY KEY,
    name             TEXT NOT NULL,
    min_spawns       INTEGER NOT NULL,
    max_spawns       INTEGER NOT NULL,
    partition_length REAL NOT NULL
);
CREATE TABLE npc_area_circle (
    area_guid   INTEGER NOT NULL,
    ord         INTEGER NOT NULL,
    x           REAL NOT NULL,
    y           REAL NOT NULL,
    z           REAL NOT NULL,
    extent      REAL NOT NULL,
    weight      REAL NOT NULL,
    max_spawns  INTEGER NOT NULL,
    PRIMARY KEY (area_guid, ord)
) WITHOUT ROWID;
CREATE TABLE npc_route (
    guid     INTEGER PRIMARY KEY,
    name     TEXT NOT NULL,
    looping  INTEGER NOT NULL,
    tx       REAL NOT NULL,
    ty       REAL NOT NULL,
    tz       REAL NOT NULL,
    qx       REAL NOT NULL,
    qy       REAL NOT NULL,
    qz       REAL NOT NULL,
    qw       REAL NOT NULL,
    sx       REAL NOT NULL,
    sy       REAL NOT NULL,
    sz       REAL NOT NULL,
    points   TEXT NOT NULL,   -- a JSON array of [x, y, z]: the spline's points relative to the actor's transform
    waypoints          TEXT NOT NULL,      -- (contentc version 8) a JSON array of [x, y, z]: the waypoint locations, relative to the actor's transform
                                           -- unless relative_city_node or relative_encounter says another frame (those routes are not placed by worldd)
    pauses             TEXT NOT NULL,      -- a JSON array of seconds: how long a patrol waits at each waypoint (shorter than waypoints: the rest wait 0)
    start_closest      INTEGER NOT NULL,   -- a patrol joins its route at the waypoint nearest to it rather than the first
    relative_city_node INTEGER NOT NULL,
    relative_encounter INTEGER NOT NULL,
    pathing_set        TEXT NOT NULL
);
CREATE TABLE npc_filter (
    guid   INTEGER PRIMARY KEY,
    name   TEXT NOT NULL,
    filter TEXT NOT NULL                        -- (contentc version 9) a FilteringOverride's PopulationFilter as JSON
);
CREATE TABLE npc_gate (
    set_guid INTEGER NOT NULL,
    source   TEXT NOT NULL,                     -- (contentc version 9) what spawns the set when it is its turn: NarrativeObjective, NarrativeQuest, EventTriggerDef,
                                                -- StoryArcPhase, BuildingLevelSpawn, Layout, CannonizationState, EventObjectiveDef, EventStageDef, EventDef, ActionResponse
    PRIMARY KEY (set_guid, source)
) WITHOUT ROWID;
CREATE TABLE vendor_def (
    guid           INTEGER PRIMARY KEY,
    name           TEXT NOT NULL,
    operations     INTEGER NOT NULL
);
CREATE TABLE vendor_def_inventory (
    vendor_guid    INTEGER NOT NULL,
    ord            INTEGER NOT NULL,
    inventory_guid INTEGER NOT NULL,
    PRIMARY KEY (vendor_guid, ord)
) WITHOUT ROWID;
CREATE TABLE vendor_inventory (
    guid   INTEGER PRIMARY KEY,
    name   TEXT NOT NULL,
    hidden INTEGER NOT NULL
);
CREATE TABLE vendor_listing (
    inventory_guid INTEGER NOT NULL,
    ord            INTEGER NOT NULL,
    item_guid      INTEGER NOT NULL,
    offers         TEXT NOT NULL,   -- a JSON array of VendorOffer guids; empty: free
    hidden         INTEGER NOT NULL,
    PRIMARY KEY (inventory_guid, ord)
) WITHOUT ROWID;
`

// PopulationTables lists the tables PopulationDDL creates, in order.
var PopulationTables = []string{
	"npc_asset", "npc_asset_loot", "npc_asset_set", "npc_asset_set_member", "npc_instance", "npc_instance_asset_set", "npc_instance_loot", "npc_set",
	"npc_set_member", "npc_world_spawn", "npc_placement", "npc_area", "npc_area_circle", "npc_route", "npc_filter", "npc_gate", "vendor_def", "vendor_def_inventory",
	"vendor_inventory", "vendor_listing",
}

// createPopulation makes the population and vendor tables in db.
func createPopulation(db *sql.DB) error {
	if _, err := db.Exec(PopulationDDL); err != nil {
		return fmt.Errorf("content: create the population tables: %w", err)
	}
	return nil
}
