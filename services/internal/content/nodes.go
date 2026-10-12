// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
)

// The city nodes (milestone "Nodes 1", stage 1; contentc version 12): what the design data holds about a node, typed. "Node" is the settlement
// (CityNodeRecord), not a resource node or a talent node; the tables say city_node, node_* and so on.
//
//	city_node             the 81 CityNodeRecordBase rows. rank is the row's index in the list sorted by guid (0 to 80): the key of the client's two small
//	                      GameState node maps, and the number a compiled expression holds a node as (a guid does not fit a float32). levelable marks the
//	                      26 nodes of the five regions that grow (a type, the world map, a zone of influence that has a node); the others are starting
//	                      areas, harbors, artisan outposts, lawless areas, dungeons and tests
//	city_node_level       the NodeLevelDetails each level of a node names (only level 3, and level 0 of the harbors, name one)
//	city_node_population  the population sets a node names itself
//	city_zoi              the 78 ZOIRecordBase rows: the zone of influence, its biome and the node it belongs to; city_zoi_adjacent the zones next to it
//	node_level_details    the 28 NodeLevelDetails with the NodeModifications of each; node_modification the 424 modifications' numbers (slots, essence,
//	                      deploy radius) and node_modification_ref what they point at (effects, mayoral abilities, commissions, recipe sets)
//	node_size             the 2 NodeSize rows: the node's radius at each level
//	child_node_rule       ChildNodeRules / ChildNodeDesiredLevelRules flattened: the most children of a level a node of a level may have
//	vassalship_def        the 2 VassalshipDef rows with the region size each level needs (vassalship_region_size)
//	node_tax_details, node_tax_table, node_tax_table_tag
//	                      the tax bounds, and the global tax tables with the tag each tax is and the predicate that makes it available
//	node_reputation_tier  the 5 reputation ranks with the tax modifier of each
//	citizenship_*, property_tax_def, election_def, node_constant
//	                      the citizenship, dues, property tax and election definitions, and the two constants records
//	node_layout*          the 70 layouts: the props (an asset set at a place), the plots per level and the population sets; node_asset_set,
//	                      node_asset_set_member, node_asset_def the 424 asset sets and 634 asset definitions (the level window and the valid cultures,
//	                      node types, seasons and biomes as bit masks of the enums' ordinals; the instances' transforms are not kept)
//	service_plot, building*  the 192 service building plots, the 71 buildings with their durations, costs and node experience, the asset sets of
//	                      each state (building_asset_state), and the populations of each node level and state (building_pop_spawn, building_level_spawn*)
//
// Enum fields are stored as their text and, where the host answers with them, as the ordinal too (aocexpr/enums.go).
//
// The transform columns (qx .. sz of node_layout_prop, node_layout_plot and node_asset_set_member) are declared with NO type on purpose: a REAL column stores a float that
// is a whole number as an integer and reads it back as +0.0, losing the sign of a negative zero, and the town replicator sends those doubles bit for bit (retail's payloads
// carry -0.0 in 29 of Joeva's 85 items).  A column with no affinity keeps the double as it came.
const NodeDDL = `
CREATE TABLE city_node (
    rank                     INTEGER NOT NULL UNIQUE,   -- the index in CityNodeRecordBase sorted by guid
    guid                     INTEGER PRIMARY KEY,
    name                     TEXT NOT NULL,             -- the record name: Verra_RVR_Joeva
    node_name                TEXT NOT NULL,             -- nodeName: Joeva
    node_type                TEXT NOT NULL,             -- Default, Scientific, Divine, Martial or Economic
    node_type_ord            INTEGER NOT NULL,          -- ENodeType
    map_name                 TEXT NOT NULL,
    x                        REAL NOT NULL,             -- location, cm
    y                        REAL NOT NULL,
    z                        REAL NOT NULL,
    yaw                      REAL NOT NULL,
    radius                   REAL NOT NULL,
    size_guid                INTEGER NOT NULL,          -- NodeSize (0: none)
    layout_guid              INTEGER NOT NULL,          -- Layout (0: none)
    zoi_guid                 INTEGER NOT NULL,          -- the ZOI whose CityNodeId is this node (0: none)
    levelable                INTEGER NOT NULL,          -- one of the 26 nodes that grow
    election_def_guid        INTEGER NOT NULL,
    tax_table_guid           INTEGER NOT NULL,
    dues_def_guid            INTEGER NOT NULL,
    property_tax_def_guid    INTEGER NOT NULL,
    vendor_def_guid          INTEGER NOT NULL,
    requires_property        INTEGER NOT NULL,          -- DoesCitizenshipRequireProperty
    mayor_min_citizens       INTEGER NOT NULL,          -- mayorVoteMinimumCitizens
    min_reputation           INTEGER NOT NULL,
    max_reputation           INTEGER NOT NULL,
    caravan_min_level        INTEGER NOT NULL,          -- ENodeLevel ordinal of MinimumLevelToTurnInCaravan
    citizenship_at_risk_secs INTEGER NOT NULL
);
CREATE INDEX city_node_by_name ON city_node (name);
CREATE TABLE city_node_level (
    node_guid    INTEGER NOT NULL,
    level        INTEGER NOT NULL,                      -- ENodeLevel
    details_guid INTEGER NOT NULL,                      -- NodeLevelDetails
    PRIMARY KEY (node_guid, level)
) WITHOUT ROWID;
CREATE TABLE city_node_population (
    node_guid          INTEGER NOT NULL,
    ord                INTEGER NOT NULL,
    population_set_guid INTEGER NOT NULL,
    PRIMARY KEY (node_guid, ord)
) WITHOUT ROWID;
CREATE TABLE city_zoi (
    guid         INTEGER PRIMARY KEY,
    name         TEXT NOT NULL,
    zoi_type     TEXT NOT NULL,                         -- HasNode, CanOwn or CannotOwn
    zoi_type_ord INTEGER NOT NULL,                      -- EZoiType
    biome        TEXT NOT NULL,                         -- Riverlands ...
    biome_ord    INTEGER NOT NULL,                      -- EBiomeType
    node_guid    INTEGER NOT NULL,                      -- CityNodeId (0: none)
    environment  TEXT NOT NULL,                         -- None, Ocean or Harbor
    has_extents  INTEGER NOT NULL,                      -- the Extents box is valid
    min_x REAL NOT NULL, min_y REAL NOT NULL, min_z REAL NOT NULL,
    max_x REAL NOT NULL, max_y REAL NOT NULL, max_z REAL NOT NULL
);
CREATE TABLE city_zoi_adjacent (
    zoi_guid      INTEGER NOT NULL,
    ord           INTEGER NOT NULL,
    adjacent_guid INTEGER NOT NULL,
    PRIMARY KEY (zoi_guid, ord)
) WITHOUT ROWID;
CREATE TABLE node_level_details (
    guid INTEGER PRIMARY KEY,
    name TEXT NOT NULL
);
CREATE TABLE node_level_details_mod (
    details_guid INTEGER NOT NULL,
    ord          INTEGER NOT NULL,
    mod_guid     INTEGER NOT NULL,
    PRIMARY KEY (details_guid, ord)
) WITHOUT ROWID;
CREATE TABLE node_modification (
    guid                       INTEGER PRIMARY KEY,
    name                       TEXT NOT NULL,
    relic_slots                INTEGER NOT NULL,        -- ActiveRelicSlots
    commission_hand_slots      INTEGER NOT NULL,
    commission_slots           INTEGER NOT NULL,
    node_essence               INTEGER NOT NULL,
    vehicle_deploy_radius      REAL NOT NULL,           -- cm
    maintenance_cost_reduction REAL NOT NULL,
    unlocks_recipes            INTEGER NOT NULL
);
CREATE TABLE node_modification_ref (
    mod_guid INTEGER NOT NULL,
    kind     TEXT NOT NULL,                             -- effect, mayoral_ability, commission or recipe_set
    ord      INTEGER NOT NULL,
    ref_guid INTEGER NOT NULL,
    PRIMARY KEY (mod_guid, kind, ord)
) WITHOUT ROWID;
CREATE TABLE node_size (
    guid INTEGER PRIMARY KEY,
    name TEXT NOT NULL
);
CREATE TABLE node_size_radius (
    size_guid INTEGER NOT NULL,
    level     INTEGER NOT NULL,
    radius    REAL NOT NULL,                            -- cm
    PRIMARY KEY (size_guid, level)
) WITHOUT ROWID;
CREATE TABLE child_node_rule (
    rules_guid   INTEGER NOT NULL,                      -- ChildNodeRules
    rules_name   TEXT NOT NULL,
    parent_level INTEGER NOT NULL,                      -- ENodeLevel of the parent
    child_level  INTEGER NOT NULL,                      -- ENodeLevel of the children
    max_children INTEGER NOT NULL,
    PRIMARY KEY (rules_guid, parent_level, child_level)
) WITHOUT ROWID;
CREATE TABLE vassalship_def (
    guid                     INTEGER PRIMARY KEY,
    name                     TEXT NOT NULL,
    rules_guid               INTEGER NOT NULL,
    max_max_level_nodes      INTEGER NOT NULL,          -- MaxNumOfMaxLevelNodes
    max_vassals              INTEGER NOT NULL,
    min_level_to_acquire     INTEGER NOT NULL,          -- ENodeLevel
    tax_percentage           REAL NOT NULL,
    tax_frequency_secs       INTEGER NOT NULL
);
CREATE TABLE vassalship_region_size (
    def_guid INTEGER NOT NULL,
    level    INTEGER NOT NULL,
    size     INTEGER NOT NULL,                          -- RequiredRegionSizeAtLevel[level]
    PRIMARY KEY (def_guid, level)
) WITHOUT ROWID;
CREATE TABLE node_tax_details (
    guid                 INTEGER PRIMARY KEY,
    name                 TEXT NOT NULL,
    default_percent      REAL NOT NULL,
    min_percent          REAL NOT NULL,
    max_percent          REAL NOT NULL,
    default_flat         REAL NOT NULL,
    min_flat             REAL NOT NULL,
    max_flat             REAL NOT NULL,
    owning_node_percent  REAL NOT NULL
);
CREATE TABLE node_tax_table (
    guid INTEGER PRIMARY KEY,
    name TEXT NOT NULL
);
CREATE TABLE node_tax_table_tag (
    table_guid   INTEGER NOT NULL,
    ord          INTEGER NOT NULL,
    tag          TEXT NOT NULL,                         -- NodeTax.GoodsTax.Marketplace
    availability TEXT NOT NULL,                         -- the predicate (AoCExpression text) that makes the tax available
    details_guid INTEGER NOT NULL,                      -- TaxDetails
    PRIMARY KEY (table_guid, ord)
) WITHOUT ROWID;
CREATE TABLE node_reputation_tier (
    guid               INTEGER PRIMARY KEY,
    name               TEXT NOT NULL,
    rank_ord           INTEGER NOT NULL,                -- ENodeReputationRank
    rank_name          TEXT NOT NULL,                   -- Peasant, Commoner, Liege, Noble or Aristocrat
    tax_modifier       REAL NOT NULL,
    can_vote_mayoral   INTEGER NOT NULL,
    can_vote_building  INTEGER NOT NULL,
    can_vote_demolition INTEGER NOT NULL
);
CREATE TABLE citizenship_node (
    guid                  INTEGER PRIMARY KEY,
    name                  TEXT NOT NULL,
    level                 INTEGER NOT NULL,
    cooldown_secs         INTEGER NOT NULL,
    dues_level_scalar     REAL NOT NULL,
    property_tax_scalar   REAL NOT NULL,
    min_buy_order_slots   INTEGER NOT NULL,
    max_buy_order_slots   INTEGER NOT NULL,
    missed_dues_increase  REAL NOT NULL,
    missed_tax_increase   REAL NOT NULL
);
CREATE TABLE citizenship_generic (
    guid                    INTEGER PRIMARY KEY,
    name                    TEXT NOT NULL,
    initial_due_cost        INTEGER NOT NULL,
    initial_fee_cost        INTEGER NOT NULL,
    missed_dues_period      INTEGER NOT NULL,
    missed_tax_period       INTEGER NOT NULL,
    missable_dues_payments  INTEGER NOT NULL,
    missable_tax_payments   INTEGER NOT NULL
);
CREATE TABLE citizenship_scalar (
    generic_guid INTEGER NOT NULL,
    kind         TEXT NOT NULL,                         -- dues or property_tax
    ord          INTEGER NOT NULL,
    guid         INTEGER NOT NULL,                      -- QuantityScalarDef
    name         TEXT NOT NULL,                         -- Dues_Medium
    threshold    REAL NOT NULL,                         -- citizens
    scalar       REAL NOT NULL,
    PRIMARY KEY (generic_guid, kind, ord)
) WITHOUT ROWID;
CREATE TABLE citizenship_dues_def (
    guid                 INTEGER PRIMARY KEY,
    name                 TEXT NOT NULL,
    base_dues_guid       INTEGER NOT NULL,              -- BaseItemValue
    rank_impact          TEXT NOT NULL,                 -- AoCExpression text
    level_impact         TEXT NOT NULL,
    non_property_owner   TEXT NOT NULL,
    due_day_of_week      TEXT NOT NULL,
    weeks                INTEGER NOT NULL,
    late_fee             REAL NOT NULL,
    overdue_days         INTEGER NOT NULL
);
CREATE TABLE property_tax_def (
    guid             INTEGER PRIMARY KEY,
    name             TEXT NOT NULL,
    item_guid        INTEGER NOT NULL,                  -- the item that pays it
    due_day_of_week  TEXT NOT NULL,
    weeks            INTEGER NOT NULL,
    overdue_days     INTEGER NOT NULL
);
CREATE TABLE election_def (
    guid                       INTEGER PRIMARY KEY,
    name                       TEXT NOT NULL,
    election_type              TEXT NOT NULL,
    registration_days          INTEGER NOT NULL,
    election_days              INTEGER NOT NULL,
    start_day                  INTEGER NOT NULL,
    start_hour                 INTEGER NOT NULL,
    end_hour                   INTEGER NOT NULL,
    results_hour               INTEGER NOT NULL,
    min_term_days              INTEGER NOT NULL,
    first_registration_secs    INTEGER NOT NULL,
    first_election_secs        INTEGER NOT NULL,
    first_wait_for_results_secs INTEGER NOT NULL,
    required_quest_guid        INTEGER NOT NULL
);
CREATE TABLE node_constant (
    name  TEXT PRIMARY KEY,                             -- RankedChoiceVoteCountLimit, NodeToPlayerReputationDecayPerHour
    value TEXT NOT NULL
);
CREATE TABLE node_layout (
    guid  INTEGER PRIMARY KEY,
    name  TEXT NOT NULL,
    level INTEGER NOT NULL
);
CREATE TABLE node_layout_prop (
    layout_guid    INTEGER NOT NULL,
    idx            INTEGER NOT NULL,                    -- the index in the layout's Props: the OwnerArrayIndex of the replicated item
    asset_set_guid INTEGER NOT NULL,
    qx NOT NULL, qy NOT NULL, qz NOT NULL, qw NOT NULL,
    tx NOT NULL, ty NOT NULL, tz NOT NULL,
    sx NOT NULL, sy NOT NULL, sz NOT NULL,
    PRIMARY KEY (layout_guid, idx)
) WITHOUT ROWID;
CREATE TABLE node_layout_plot (
    layout_guid INTEGER NOT NULL,
    level       INTEGER NOT NULL,                       -- PlotsPerNodeLevel's key
    idx         INTEGER NOT NULL,
    plot_guid   INTEGER NOT NULL,                       -- ServiceBuildingPlot
    qx NOT NULL, qy NOT NULL, qz NOT NULL, qw NOT NULL,
    tx NOT NULL, ty NOT NULL, tz NOT NULL,
    sx NOT NULL, sy NOT NULL, sz NOT NULL,
    PRIMARY KEY (layout_guid, level, idx)
) WITHOUT ROWID;
CREATE TABLE node_layout_population (
    layout_guid         INTEGER NOT NULL,
    ord                 INTEGER NOT NULL,
    population_set_guid INTEGER NOT NULL,
    PRIMARY KEY (layout_guid, ord)
) WITHOUT ROWID;
CREATE TABLE node_asset_set (
    guid INTEGER PRIMARY KEY,
    name TEXT NOT NULL
);
CREATE TABLE node_asset_set_member (
    set_guid INTEGER NOT NULL,
    ord      INTEGER NOT NULL,
    def_guid INTEGER NOT NULL,                          -- NodeAssetDefinition
    qx NOT NULL, qy NOT NULL, qz NOT NULL, qw NOT NULL,
    tx NOT NULL, ty NOT NULL, tz NOT NULL,
    sx NOT NULL, sy NOT NULL, sz NOT NULL,
    PRIMARY KEY (set_guid, ord)
) WITHOUT ROWID;
CREATE TABLE node_asset_def (
    guid        INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    level_min   INTEGER NOT NULL,                       -- NodeLevelMinimum
    level_max   INTEGER NOT NULL,
    weight      REAL NOT NULL,
    all_biomes  INTEGER NOT NULL,                       -- bAllBiomes: ValidBiomes is not read
    cultures    INTEGER NOT NULL,                       -- ValidCultures as a mask of ECharacterRace ordinals (bit n = ordinal n)
    node_types  INTEGER NOT NULL,                       -- ValidNodeTypes: ENodeType
    seasons     INTEGER NOT NULL,                       -- ValidSeasons: ESeason
    biomes      INTEGER NOT NULL                        -- ValidBiomes: EBiomeType
);
CREATE TABLE node_asset_def_tag (
    def_guid INTEGER NOT NULL,
    ord      INTEGER NOT NULL,
    tag      TEXT NOT NULL,                             -- ValidGameplayTags
    PRIMARY KEY (def_guid, ord)
) WITHOUT ROWID;
CREATE TABLE service_plot (
    guid                INTEGER PRIMARY KEY,
    name                TEXT NOT NULL,
    pre_construct       INTEGER NOT NULL,
    starting_state      INTEGER NOT NULL,               -- EBuildingState
    empty_building_guid INTEGER NOT NULL
);
CREATE TABLE service_plot_building (
    plot_guid     INTEGER NOT NULL,
    ord           INTEGER NOT NULL,
    building_guid INTEGER NOT NULL,
    PRIMARY KEY (plot_guid, ord)
) WITHOUT ROWID;
CREATE TABLE building (
    guid                    INTEGER PRIMARY KEY,
    name                    TEXT NOT NULL,
    building_type           TEXT NOT NULL,              -- EServiceBuildingType: Civic, Artisan ...
    asset_sets_guid         INTEGER NOT NULL,           -- BuildingNodeAssetSets
    contribution_phases_guid INTEGER NOT NULL,
    completion_node_xp      REAL NOT NULL,              -- CompletionNodeExperience
    can_time_out            INTEGER NOT NULL,
    construction_ms         INTEGER NOT NULL,
    demolition_ms           INTEGER NOT NULL,
    demolition_vote_ms      INTEGER NOT NULL,
    destroyed_ms            INTEGER NOT NULL,
    repair_ms               INTEGER NOT NULL,
    node_cost_guid          INTEGER NOT NULL,
    player_cost_guid        INTEGER NOT NULL,
    maintenance_cost_guid   INTEGER NOT NULL,
    disarray_repair_guid    INTEGER NOT NULL,
    damaged_repair_guid     INTEGER NOT NULL,
    expansion_tree_guid     INTEGER NOT NULL,
    in_building_manager     INTEGER NOT NULL
);
CREATE TABLE building_asset_sets (
    guid INTEGER PRIMARY KEY,                           -- BuildingNodeAssetSets
    name TEXT NOT NULL
);
CREATE TABLE building_asset_state (
    sets_guid      INTEGER NOT NULL,
    state          INTEGER NOT NULL,                    -- EBuildingState
    ord            INTEGER NOT NULL,
    asset_set_guid INTEGER NOT NULL,
    PRIMARY KEY (sets_guid, state, ord)
) WITHOUT ROWID;
CREATE TABLE building_pop_spawn (
    building_guid INTEGER NOT NULL,
    level         INTEGER NOT NULL,                     -- ENodeLevel
    spawn_guid    INTEGER NOT NULL,                     -- BuildingLevelSpawn
    PRIMARY KEY (building_guid, level)
) WITHOUT ROWID;
CREATE TABLE building_level_spawn (
    guid INTEGER PRIMARY KEY,
    name TEXT NOT NULL
);
CREATE TABLE building_level_spawn_pop (
    spawn_guid          INTEGER NOT NULL,
    state               INTEGER NOT NULL,               -- EBuildingState
    ord                 INTEGER NOT NULL,
    population_set_guid INTEGER NOT NULL,
    PRIMARY KEY (spawn_guid, state, ord)
) WITHOUT ROWID;
`

// NodeTables are the tables NodeDDL creates, in the DDL's order.
var NodeTables = []string{"city_node", "city_node_level", "city_node_population", "city_zoi", "city_zoi_adjacent", "node_level_details", "node_level_details_mod",
	"node_modification", "node_modification_ref", "node_size", "node_size_radius", "child_node_rule", "vassalship_def", "vassalship_region_size",
	"node_tax_details", "node_tax_table", "node_tax_table_tag", "node_reputation_tier", "citizenship_node", "citizenship_generic", "citizenship_scalar",
	"citizenship_dues_def", "property_tax_def", "election_def", "node_constant", "node_layout", "node_layout_prop", "node_layout_plot",
	"node_layout_population", "node_asset_set", "node_asset_set_member", "node_asset_def", "node_asset_def_tag", "service_plot", "service_plot_building",
	"building", "building_asset_sets", "building_asset_state", "building_pop_spawn", "building_level_spawn", "building_level_spawn_pop"}

// The levels of a node (ENodeLevel's ordinals).
const (
	NodeLevelWilderness = 0
	NodeLevelVillage    = 3
	NodeLevelTown       = 4
)

// CityNode is a row of city_node.
type CityNode struct {
	Rank                  int
	Guid                  int64
	Name                  string // the record name, Verra_RVR_Joeva
	NodeName              string // Joeva
	NodeType              string
	NodeTypeOrd           int
	MapName               string
	X, Y, Z, Yaw          float64
	Radius                float64
	SizeGuid              int64
	LayoutGuid            int64
	ZoiGuid               int64
	Levelable             bool
	ElectionDefGuid       int64
	TaxTableGuid          int64
	DuesDefGuid           int64
	PropertyTaxDefGuid    int64
	VendorDefGuid         int64
	RequiresProperty      bool
	MayorMinCitizens      int
	MinReputation         int
	MaxReputation         int
	CaravanMinLevel       int
	CitizenshipAtRiskSecs int64
}

// C-Y-B.CoA

// CityZoi is a row of city_zoi with its neighbours.
type CityZoi struct {
	Guid        int64
	Name        string
	ZoiType     string
	ZoiTypeOrd  int
	Biome       string
	BiomeOrd    int
	NodeGuid    int64
	Environment string
	HasExtents  bool
	Min, Max    [3]float64
	Adjacent    []int64
}

// NodeLevelDetails is a NodeLevelDetails row with the modifications it names, in order.
type NodeLevelDetails struct {
	Guid          int64
	Name          string
	Modifications []int64
}

// NodeModification is a row of node_modification.
type NodeModification struct {
	Guid                     int64
	Name                     string
	RelicSlots               int
	CommissionHandSlots      int
	CommissionSlots          int
	NodeEssence              int
	VehicleDeployRadius      float64
	MaintenanceCostReduction float64
	UnlocksRecipes           bool
}

// ChildNodeRule is a row of child_node_rule: a node of ParentLevel may have MaxChildren children of ChildLevel.
type ChildNodeRule struct {
	RulesGuid   int64
	RulesName   string
	ParentLevel int
	ChildLevel  int
	MaxChildren int
}

// VassalshipDef is a vassalship_def row with its region sizes by level.
type VassalshipDef struct {
	Guid                 int64
	Name                 string
	RulesGuid            int64
	MaxMaxLevelNodes     int
	MaxVassals           int
	MinLevelToAcquire    int
	TaxPercentage        float64
	TaxFrequencySecs     int64
	RequiredRegionSizeAt []int // by level
}

func createNodes(db *sql.DB) error {
	if _, err := db.Exec(NodeDDL); err != nil {
		return fmt.Errorf("content: create the city node tables: %w", err)
	}
	return nil
}

// CityNodes reads every city node, in rank order.
func (d *DB) CityNodes() ([]CityNode, error) {
	rows, err := d.sql.Query(`SELECT rank, guid, name, node_name, node_type, node_type_ord, map_name, x, y, z, yaw, radius, size_guid, layout_guid, zoi_guid,
		levelable, election_def_guid, tax_table_guid, dues_def_guid, property_tax_def_guid, vendor_def_guid, requires_property, mayor_min_citizens,
		min_reputation, max_reputation, caravan_min_level, citizenship_at_risk_secs FROM city_node ORDER BY rank`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []CityNode
	for rows.Next() {
		var n CityNode
		if err := rows.Scan(&n.Rank, &n.Guid, &n.Name, &n.NodeName, &n.NodeType, &n.NodeTypeOrd, &n.MapName, &n.X, &n.Y, &n.Z, &n.Yaw, &n.Radius, &n.SizeGuid,
			&n.LayoutGuid, &n.ZoiGuid, &n.Levelable, &n.ElectionDefGuid, &n.TaxTableGuid, &n.DuesDefGuid, &n.PropertyTaxDefGuid, &n.VendorDefGuid,
			&n.RequiresProperty, &n.MayorMinCitizens, &n.MinReputation, &n.MaxReputation, &n.CaravanMinLevel, &n.CitizenshipAtRiskSecs); err != nil {
			return nil, err
		}
		out = append(out, n)
	}
	return out, rows.Err()
}

// CityZois reads every zone of influence with its neighbours, by guid.
func (d *DB) CityZois() ([]CityZoi, error) {
	rows, err := d.sql.Query(`SELECT guid, name, zoi_type, zoi_type_ord, biome, biome_ord, node_guid, environment, has_extents,
		min_x, min_y, min_z, max_x, max_y, max_z FROM city_zoi ORDER BY guid`)
	if err != nil {
		return nil, err
	}
	var out []CityZoi
	for rows.Next() {
		var z CityZoi
		if err := rows.Scan(&z.Guid, &z.Name, &z.ZoiType, &z.ZoiTypeOrd, &z.Biome, &z.BiomeOrd, &z.NodeGuid, &z.Environment, &z.HasExtents,
			&z.Min[0], &z.Min[1], &z.Min[2], &z.Max[0], &z.Max[1], &z.Max[2]); err != nil {
			rows.Close()
			return nil, err
		}
		out = append(out, z)
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}
	adj, err := d.sql.Query(`SELECT zoi_guid, adjacent_guid FROM city_zoi_adjacent ORDER BY zoi_guid, ord`)
	if err != nil {
		return nil, err
	}
	defer adj.Close()
	by := map[int64][]int64{}
	for adj.Next() {
		var z, a int64
		if err := adj.Scan(&z, &a); err != nil {
			return nil, err
		}
		by[z] = append(by[z], a)
	}
	if err := adj.Err(); err != nil {
		return nil, err
	}
	for i := range out {
		out[i].Adjacent = by[out[i].Guid]
	}
	return out, nil
}

// C O A / Cyb

// CityNodeLevelDetails reads what each node names for each level: node guid -> level -> NodeLevelDetails guid.
func (d *DB) CityNodeLevelDetails() (map[int64]map[int]int64, error) {
	rows, err := d.sql.Query(`SELECT node_guid, level, details_guid FROM city_node_level ORDER BY node_guid, level`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[int64]map[int]int64{}
	for rows.Next() {
		var node, details int64
		var level int
		if err := rows.Scan(&node, &level, &details); err != nil {
			return nil, err
		}
		if out[node] == nil {
			out[node] = map[int]int64{}
		}
		out[node][level] = details
	}
	return out, rows.Err()
}

// NodeLevelDetailsList reads the NodeLevelDetails rows with their modifications, by guid.
func (d *DB) NodeLevelDetailsList() ([]NodeLevelDetails, error) {
	rows, err := d.sql.Query(`SELECT guid, name FROM node_level_details ORDER BY guid`)
	if err != nil {
		return nil, err
	}
	var out []NodeLevelDetails
	for rows.Next() {
		var n NodeLevelDetails
		if err := rows.Scan(&n.Guid, &n.Name); err != nil {
			rows.Close()
			return nil, err
		}
		out = append(out, n)
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}
	mods, err := d.sql.Query(`SELECT details_guid, mod_guid FROM node_level_details_mod ORDER BY details_guid, ord`)
	if err != nil {
		return nil, err
	}
	defer mods.Close()
	by := map[int64][]int64{}
	for mods.Next() {
		var a, b int64
		if err := mods.Scan(&a, &b); err != nil {
			return nil, err
		}
		by[a] = append(by[a], b)
	}
	if err := mods.Err(); err != nil {
		return nil, err
	}
	for i := range out {
		out[i].Modifications = by[out[i].Guid]
	}
	return out, nil
}

// NodeModifications reads every node modification, by guid.
func (d *DB) NodeModifications() ([]NodeModification, error) {
	rows, err := d.sql.Query(`SELECT guid, name, relic_slots, commission_hand_slots, commission_slots, node_essence, vehicle_deploy_radius,
		maintenance_cost_reduction, unlocks_recipes FROM node_modification ORDER BY guid`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []NodeModification
	for rows.Next() {
		var m NodeModification
		if err := rows.Scan(&m.Guid, &m.Name, &m.RelicSlots, &m.CommissionHandSlots, &m.CommissionSlots, &m.NodeEssence, &m.VehicleDeployRadius,
			&m.MaintenanceCostReduction, &m.UnlocksRecipes); err != nil {
			return nil, err
		}
		out = append(out, m)
	}
	return out, rows.Err()
}

// == C | O | A | Cyb ==

// ChildNodeRules reads the flattened child node rules.
func (d *DB) ChildNodeRules() ([]ChildNodeRule, error) {
	rows, err := d.sql.Query(`SELECT rules_guid, rules_name, parent_level, child_level, max_children FROM child_node_rule
		ORDER BY rules_guid, parent_level, child_level`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []ChildNodeRule
	for rows.Next() {
		var r ChildNodeRule
		if err := rows.Scan(&r.RulesGuid, &r.RulesName, &r.ParentLevel, &r.ChildLevel, &r.MaxChildren); err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	return out, rows.Err()
}

// VassalshipDefs reads the vassalship definitions with their region sizes.
func (d *DB) VassalshipDefs() ([]VassalshipDef, error) {
	rows, err := d.sql.Query(`SELECT guid, name, rules_guid, max_max_level_nodes, max_vassals, min_level_to_acquire, tax_percentage, tax_frequency_secs
		FROM vassalship_def ORDER BY guid`)
	if err != nil {
		return nil, err
	}
	var out []VassalshipDef
	for rows.Next() {
		var v VassalshipDef
		if err := rows.Scan(&v.Guid, &v.Name, &v.RulesGuid, &v.MaxMaxLevelNodes, &v.MaxVassals, &v.MinLevelToAcquire, &v.TaxPercentage, &v.TaxFrequencySecs); err != nil {
			rows.Close()
			return nil, err
		}
		out = append(out, v)
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}
	sizes, err := d.sql.Query(`SELECT def_guid, level, size FROM vassalship_region_size ORDER BY def_guid, level`)
	if err != nil {
		return nil, err
	}
	defer sizes.Close()
	by := map[int64][]int{}
	for sizes.Next() {
		var g int64
		var level, size int
		if err := sizes.Scan(&g, &level, &size); err != nil {
			return nil, err
		}
		for len(by[g]) <= level {
			by[g] = append(by[g], 0)
		}
		by[g][level] = size
	}
	if err := sizes.Err(); err != nil {
		return nil, err
	}
	for i := range out {
		out[i].RequiredRegionSizeAt = by[out[i].Guid]
	}
	return out, nil
}
