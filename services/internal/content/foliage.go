// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
)

// Foliage gathering (milestone "Gathering stage 3", part "Foliage gathering"; contentc version 15): what the design data says about the painted trees, plants and
// stones of the world, and about every gathering interaction (the nodes', the foliage's, the Hunting corpses', fishing's). The raw records are in `record` as before.
//
//	foliage_def             the 402 GatheringFoliageData: how long an emptied instance stays empty (EmptyDuration), the state it is painted in, the states it can be
//	                        harvested and grow in, and the GatheringFoliageState of each of its states (Infancy 0, Juvenile 1, Adult 2; 0 = the def has none)
//	foliage_state           the 743 GatheringFoliageState: how long an instance stays in the state before it grows into the next (LifecycleDuration; 0 = it stays),
//	                        the interaction harvesting it in that state (a GatheringInteractionData; 0 = none: a sapling), the distance and angle the interaction
//	                        overrides (-1 = the interaction's own) and the collision radius
//	gather_interaction      the 236 GatheringInteractionData: profession, certification, tool slot, base duration and the compiled duration and wear formulas, the
//	                        reward table and the state the harvest leaves the foliage in (Empty). The range comes from the PlayerInteractionDef named like it
//	                        (else the first by guid that names it; 0 = no def: radius 0, tolerance 0, combat allowed)
//	gather_interaction_def  the 194 PlayerInteractionDefs that name a GatheringInteractionData, by their own guid: the 53 Hunting_<X> defs of the huntable creatures
//	                        (NpcAsset.InteractionDef) and the nodes' and foliage's ranges
//
// The state ordinals are EFoliageState's: Infancy 0, Juvenile 1, Adult 2, Empty 3 (the captured IndexedStates carry 3 for a harvested instance and 0 and 1 for a growing one;
// FoliageData's clear writes 3).
const FoliageDDL = `
CREATE TABLE foliage_def (
    guid               INTEGER PRIMARY KEY,  -- GatheringFoliageData
    name               TEXT NOT NULL,        -- Tree_Oak_Large_1
    empty_duration     REAL NOT NULL,        -- seconds an instance stays Empty after a harvest
    dynamic_guid       INTEGER NOT NULL,     -- DynamicEmptyDurationDefId, a gather_dynamic guid (0: none); the seasonal factor is not applied
    resource_type_guid INTEGER NOT NULL,     -- GatheringResourceType (0: none)
    paint_state        INTEGER NOT NULL,     -- the state the map paints the instances in (what an instance returns to)
    gatherable_state   INTEGER NOT NULL,
    min_state          INTEGER NOT NULL,
    max_state          INTEGER NOT NULL,
    state0_guid        INTEGER NOT NULL,     -- the GatheringFoliageState of Infancy (0: none)
    state1_guid        INTEGER NOT NULL,     -- ... of Juvenile
    state2_guid        INTEGER NOT NULL      -- ... of Adult
);
CREATE INDEX foliage_def_by_name ON foliage_def (name);
CREATE TABLE foliage_state (
    guid               INTEGER PRIMARY KEY,  -- GatheringFoliageState
    name               TEXT NOT NULL,
    lifecycle_duration REAL NOT NULL,        -- seconds in the state before the next one (0: permanent)
    dynamic_guid       INTEGER NOT NULL,     -- DynamicLifecycleDefId (0: none); the seasonal factor is not applied
    interaction_guid   INTEGER NOT NULL,     -- the GatheringInteractionData that harvests the instance in this state (0: none)
    override_distance  REAL NOT NULL,        -- OverrideInteractionDistance, cm (-1: the interaction's)
    override_angle     REAL NOT NULL,        -- OverrideInteractionAngle, degrees (-1: the interaction's)
    collision_radius   REAL NOT NULL         -- cm
);
CREATE TABLE gather_interaction (
    guid              INTEGER PRIMARY KEY,   -- GatheringInteractionData
    name              TEXT NOT NULL,         -- Tree_Oak_Large, Hunting_Wolf
    target_state      INTEGER NOT NULL,      -- TargetFoliageState: the state the harvest leaves the foliage in (Empty)
    profession_guid   INTEGER NOT NULL,
    cert_guid         INTEGER NOT NULL,
    cert_tier         INTEGER NOT NULL,      -- 0 Novice ... 4 GrandMaster
    tool_slot         INTEGER NOT NULL,      -- the EEquipSlot of the tool (equip_slot.slot); -1 when the data names none
    base_duration     REAL NOT NULL,         -- seconds
    duration_expr     INTEGER NOT NULL,      -- the compiled GatheringDurationFormula (ab_expr id, 0: none)
    durability_expr   INTEGER NOT NULL,      -- the compiled DurabilityLossFormula (ab_expr id, 0: none)
    reward_guid       INTEGER NOT NULL,      -- the RewardTable (loot_table.guid)
    interaction_guid  INTEGER NOT NULL,      -- the PlayerInteractionDef that carries the range (0: none)
    interact_radius   REAL NOT NULL,         -- cm (0 with no def)
    interact_tolerance REAL NOT NULL,        -- cm
    allow_in_combat   INTEGER NOT NULL       -- 1 with no def
);
CREATE INDEX gather_interaction_by_name ON gather_interaction (name);
CREATE TABLE gather_interaction_def (
    interaction_guid   INTEGER PRIMARY KEY,  -- PlayerInteractionDef
    name               TEXT NOT NULL,        -- Hunting_Wolf
    gather_guid        INTEGER NOT NULL,     -- the GatheringInteractionData it names (one is no record)
    interact_radius    REAL NOT NULL,
    interact_tolerance REAL NOT NULL,
    discovery_distance REAL NOT NULL,
    allow_in_combat    INTEGER NOT NULL
);
CREATE INDEX gather_interaction_def_by_gather ON gather_interaction_def (gather_guid);
`

// FoliageTables are the tables FoliageDDL creates, in the DDL's order.
var FoliageTables = []string{"foliage_def", "foliage_state", "gather_interaction", "gather_interaction_def"}

// built with C-Y-B and CoA

func createFoliage(db *sql.DB) error {
	if _, err := db.Exec(FoliageDDL); err != nil {
		return fmt.Errorf("content: create the foliage tables: %w", err)
	}
	return nil
}

// EFoliageState: the states of a tree or plant, as the client's IndexedStates and FoliageData carry them.
const (
	StateInfancy  = 0
	StateJuvenile = 1
	StateAdult    = 2
	StateEmpty    = 3
)

// FoliageStateNames are the EFoliageState names by ordinal.
var FoliageStateNames = [4]string{"Infancy", "Juvenile", "Adult", "Empty"}

// FoliageDef is a foliage_def row.
type FoliageDef struct {
	Guid            int64
	Name            string
	EmptyDuration   float64
	DynamicGuid     int64
	ResourceType    int64
	PaintState      int
	GatherableState int
	MinState        int
	MaxState        int
	StateGuids      [3]int64 // by state: Infancy, Juvenile, Adult
}

// FoliageDefs reads every foliage def, by guid.
func (d *DB) FoliageDefs() ([]FoliageDef, error) {
	rows, err := d.sql.Query(`SELECT guid, name, empty_duration, dynamic_guid, resource_type_guid, paint_state, gatherable_state, min_state, max_state, state0_guid, state1_guid, state2_guid
		FROM foliage_def ORDER BY guid`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []FoliageDef
	for rows.Next() {
		var f FoliageDef
		if err := rows.Scan(&f.Guid, &f.Name, &f.EmptyDuration, &f.DynamicGuid, &f.ResourceType, &f.PaintState, &f.GatherableState, &f.MinState, &f.MaxState,
			&f.StateGuids[0], &f.StateGuids[1], &f.StateGuids[2]); err != nil {
			return nil, err
		}
		out = append(out, f)
	}
	return out, rows.Err()
}

// FoliageState is a foliage_state row.
type FoliageState struct {
	Guid              int64
	Name              string
	LifecycleDuration float64
	DynamicGuid       int64
	InteractionGuid   int64
	OverrideDistance  float64
	OverrideAngle     float64
	CollisionRadius   float64
}

// (Cyb : C.O.A)

// FoliageStates reads every foliage state, by guid.
func (d *DB) FoliageStates() ([]FoliageState, error) {
	rows, err := d.sql.Query(`SELECT guid, name, lifecycle_duration, dynamic_guid, interaction_guid, override_distance, override_angle, collision_radius FROM foliage_state ORDER BY guid`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []FoliageState
	for rows.Next() {
		var s FoliageState
		if err := rows.Scan(&s.Guid, &s.Name, &s.LifecycleDuration, &s.DynamicGuid, &s.InteractionGuid, &s.OverrideDistance, &s.OverrideAngle, &s.CollisionRadius); err != nil {
			return nil, err
		}
		out = append(out, s)
	}
	return out, rows.Err()
}

// GatherInteraction is a gather_interaction row.
type GatherInteraction struct {
	Guid              int64
	Name              string
	TargetState       int
	ProfessionGuid    int64
	CertGuid          int64
	CertTier          int
	ToolSlot          int
	BaseDuration      float64
	DurationExpr      int64
	DurabilityExpr    int64
	RewardGuid        int64
	InteractionGuid   int64
	InteractRadius    float64
	InteractTolerance float64
	AllowInCombat     bool
}

// GatherInteractions reads every gathering interaction, by guid.
func (d *DB) GatherInteractions() ([]GatherInteraction, error) {
	rows, err := d.sql.Query(`SELECT guid, name, target_state, profession_guid, cert_guid, cert_tier, tool_slot, base_duration, duration_expr, durability_expr, reward_guid,
		interaction_guid, interact_radius, interact_tolerance, allow_in_combat FROM gather_interaction ORDER BY guid`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []GatherInteraction
	for rows.Next() {
		var g GatherInteraction
		var combat int
		if err := rows.Scan(&g.Guid, &g.Name, &g.TargetState, &g.ProfessionGuid, &g.CertGuid, &g.CertTier, &g.ToolSlot, &g.BaseDuration, &g.DurationExpr, &g.DurabilityExpr, &g.RewardGuid,
			&g.InteractionGuid, &g.InteractRadius, &g.InteractTolerance, &combat); err != nil {
			return nil, err
		}
		g.AllowInCombat = combat != 0
		out = append(out, g)
	}
	return out, rows.Err()
}

// GatherInteractionDef is a gather_interaction_def row.
type GatherInteractionDef struct {
	Guid              int64
	Name              string
	GatherGuid        int64
	InteractRadius    float64
	InteractTolerance float64
	DiscoveryDistance float64
	AllowInCombat     bool
}

// GatherInteractionDefs reads every PlayerInteractionDef that names a gathering interaction, by guid.
func (d *DB) GatherInteractionDefs() ([]GatherInteractionDef, error) {
	rows, err := d.sql.Query(`SELECT interaction_guid, name, gather_guid, interact_radius, interact_tolerance, discovery_distance, allow_in_combat FROM gather_interaction_def ORDER BY interaction_guid`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []GatherInteractionDef
	for rows.Next() {
		var p GatherInteractionDef
		var combat int
		if err := rows.Scan(&p.Guid, &p.Name, &p.GatherGuid, &p.InteractRadius, &p.InteractTolerance, &p.DiscoveryDistance, &combat); err != nil {
			return nil, err
		}
		p.AllowInCombat = combat != 0
		out = append(out, p)
	}
	return out, rows.Err()
}
