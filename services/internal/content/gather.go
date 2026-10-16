// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
)

// Gathering (milestone "Gathering stage 2", part "Gathering data"; contentc version 13): what the design data says about harvesting a node and about
// the professions, typed. The raw records are in `record` as before; these tables join them.
//
//	gather_def           the 339 GatherableActorDefs, each joined to its GatheringFoliageData (EmptyDuration, the seasonal def), its
//	                     PlayerInteractionDef (InteractRadius, InteractTolerance, the flags) and its GatheringInteractionData (profession, tool slot,
//	                     certification, base duration, the compiled duration and durability formulas, the reward table). Every def has all three
//	gather_dynamic       the 34 GatheringDynamicDefs: the multiplier of the empty duration by season, day and night (ESeason ordinals)
//	gather_tool          the 87 tool items with the GatheringToolData they name: the profession and the certification a tool is for, and the slot it is
//	                     worn in (the item's first EquipSlot: Pickaxe 26, Axe 29, Sickle 33, FishingRod 35, Bow 38)
//	item_variation       the 47 ItemVariations: the compiled Quality formula (Artisan_Gathering_<Prof>_<Tool>_Rarity[_T1_Only] for the harvest rewards) and
//	                     the Durability formula, as ab_expr ids (0: none)
//	profession           the 22 CharacterProfessions: category, leveling type and skill tree
//	certification_level  the 5 CertificationLevels: the tier (Novice 0 ... GrandMaster 4), the profession levels it spans and the most professions a
//	                     character may hold at it (Apprentice 5, Journeyman 4, Master 3, GrandMaster 2; Novice has none)
//	action_certification the 98 SetCertificationLevel ActionResponses: the profession and whether the action promotes or demotes one tier
//
// The profession level-up curves are in stat_curve (Gathering_LevelUpCurve, Processing_LevelUpCurve, Crafting_LevelUpCurve) and in leveling_curve (the 22
// profession leveling types): contentc reads them from dist/content/artisan_curves.tsv, which `mapc artisan-curves` writes from the game's paks (the design
// database holds only a placeholder). item_use/reward_xp rows of the experience type Profession point at their compiled expression (reward_xp.expr).
const GatherDDL = `
CREATE TABLE gather_def (
    guid                      INTEGER PRIMARY KEY,  -- GatherableActorDef
    name                      TEXT NOT NULL,        -- Tree_Oak_Large_2
    foliage_guid              INTEGER NOT NULL,     -- GatheringFoliageData
    empty_duration            REAL NOT NULL,        -- the foliage data's EmptyDuration, seconds
    override_respawn          REAL NOT NULL,        -- the actor def's OverrideRespawnDuration, seconds (0: none)
    dynamic_guid              INTEGER NOT NULL,     -- DynamicEmptyDurationDefId, a gather_dynamic row (0: none)
    resource_type_guid        INTEGER NOT NULL,     -- GatheringResourceType (0: none)
    gatherable_state          TEXT NOT NULL,        -- EFoliageState the node can be gathered at: Juvenile ...
    interaction_guid          INTEGER NOT NULL,     -- PlayerInteractionDef
    interact_radius           REAL NOT NULL,        -- cm
    interact_tolerance        REAL NOT NULL,        -- cm
    discovery_distance        REAL NOT NULL,        -- cm
    channel_duration          REAL NOT NULL,        -- the interaction's own ChannelDuration (a default: the harvest takes duration_expr)
    allow_in_combat           INTEGER NOT NULL,
    single_use                INTEGER NOT NULL,
    require_los               INTEGER NOT NULL,
    cancel_if_predicate_fails INTEGER NOT NULL,     -- CancelChannelIfActivePredicateEverFails
    gather_guid               INTEGER NOT NULL,     -- GatheringInteractionData
    profession_guid           INTEGER NOT NULL,     -- the profession (profession.guid)
    tool_slot                 INTEGER NOT NULL,     -- the EEquipSlot of the tool (equip_slot.slot); -1 when the data names none
    cert_guid                 INTEGER NOT NULL,     -- the certification level the interaction needs (certification_level.guid)
    cert_tier                 INTEGER NOT NULL,     -- its tier, 0 Novice ... 4 GrandMaster
    base_duration             REAL NOT NULL,        -- BaseGatheringDuration, seconds (GetGatheringInteractionBaseDuration())
    duration_expr             INTEGER NOT NULL,     -- the GatheringDurationFormula as an ab_expr id (0: none): the harvest's seconds
    durability_expr           INTEGER NOT NULL,     -- the DurabilityLossFormula as an ab_expr id (0: none): the tool's wear
    reward_guid               INTEGER NOT NULL      -- the RewardTable (loot_table.guid)
);
CREATE INDEX gather_def_by_name ON gather_def (name);
CREATE TABLE gather_dynamic (
    guid       INTEGER NOT NULL,
    name       TEXT NOT NULL,
    season     INTEGER NOT NULL,                    -- ESeason: Winter 0, Spring 1, Summer 2, Fall 3
    day_mult   REAL NOT NULL,
    night_mult REAL NOT NULL,
    PRIMARY KEY (guid, season)
) WITHOUT ROWID;
CREATE TABLE gather_tool (
    item_guid            INTEGER PRIMARY KEY,
    item_name            TEXT NOT NULL,             -- Gear_Artisan_Mining_Tool_Pickaxe_T1_0
    tool_data_guid       INTEGER NOT NULL,          -- GatheringToolData
    tool_data_name       TEXT NOT NULL,             -- Mining_Tool_Pickaxe_Novice
    profession_guid      INTEGER NOT NULL,
    cert_guid            INTEGER NOT NULL,
    cert_tier            INTEGER NOT NULL,
    tool_slot            INTEGER NOT NULL,          -- the item's first EquipSlot (equip_slot.slot)
    required_profession  INTEGER NOT NULL,          -- the item's own RequiredProfessionId (0: none)
    required_cert        INTEGER NOT NULL           -- the item's own RequiredCertificationLevelId (0: none)
);
CREATE TABLE item_variation (
    guid            INTEGER PRIMARY KEY,
    name            TEXT NOT NULL,
    quality_expr    INTEGER NOT NULL,               -- ab_expr id of the Quality formula (0: none)
    durability_expr INTEGER NOT NULL                -- ab_expr id of the Durability formula (0: none)
);
CREATE TABLE profession (
    guid              INTEGER PRIMARY KEY,
    name              TEXT NOT NULL,                -- Mining, Weaponsmithing ...
    category          TEXT NOT NULL,                -- Gathering, Processing or Crafting
    leveling_type_guid INTEGER NOT NULL,            -- leveling_type.guid
    skill_tree_guid   INTEGER NOT NULL
);
CREATE TABLE certification_level (
    guid              INTEGER PRIMARY KEY,
    name              TEXT NOT NULL,                -- Novice, Apprentice, Journeyman, Master, GrandMaster
    tier              INTEGER NOT NULL,             -- CertificationTier 0 to 4
    min_level         INTEGER NOT NULL,
    max_level         INTEGER NOT NULL,             -- a profession's level stops here until it is promoted
    has_max_allowable INTEGER NOT NULL,
    max_allowable     INTEGER NOT NULL              -- the most professions a character may hold at this level (0 when has_max_allowable is 0)
);
CREATE TABLE action_certification (
    action_guid     INTEGER PRIMARY KEY,
    name            TEXT NOT NULL,
    profession_guid INTEGER NOT NULL,               -- the profession (0: the action names none)
    action          TEXT NOT NULL                   -- Promote or Demote: one tier up or down
);
`

// GatherTables are the tables GatherDDL creates, in the DDL's order.
var GatherTables = []string{"gather_def", "gather_dynamic", "gather_tool", "item_variation", "profession", "certification_level", "action_certification"}

func createGather(db *sql.DB) error {
	if _, err := db.Exec(GatherDDL); err != nil {
		return fmt.Errorf("content: create the gathering tables: %w", err)
	}
	return nil
}

// GatherDef is a gather_def row.
type GatherDef struct {
	Guid              int64
	Name              string
	FoliageGuid       int64
	EmptyDuration     float64
	OverrideRespawn   float64
	DynamicGuid       int64
	ResourceTypeGuid  int64
	GatherableState   string
	InteractionGuid   int64
	InteractRadius    float64
	InteractTolerance float64
	DiscoveryDistance float64
	ChannelDuration   float64
	AllowInCombat     bool
	SingleUse         bool
	RequireLOS        bool
	CancelIfPredFails bool
	GatherGuid        int64
	ProfessionGuid    int64
	ToolSlot          int
	CertGuid          int64
	CertTier          int
	BaseDuration      float64
	DurationExpr      int64
	DurabilityExpr    int64
	RewardGuid        int64
}

// GatherDefs reads every gatherable actor def, by guid.
func (d *DB) GatherDefs() ([]GatherDef, error) {
	rows, err := d.sql.Query(`SELECT guid, name, foliage_guid, empty_duration, override_respawn, dynamic_guid, resource_type_guid, gatherable_state, interaction_guid,
		interact_radius, interact_tolerance, discovery_distance, channel_duration, allow_in_combat, single_use, require_los, cancel_if_predicate_fails, gather_guid,
		profession_guid, tool_slot, cert_guid, cert_tier, base_duration, duration_expr, durability_expr, reward_guid FROM gather_def ORDER BY guid`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []GatherDef
	for rows.Next() {
		var g GatherDef
		var combat, single, los, cancel int
		if err := rows.Scan(&g.Guid, &g.Name, &g.FoliageGuid, &g.EmptyDuration, &g.OverrideRespawn, &g.DynamicGuid, &g.ResourceTypeGuid, &g.GatherableState, &g.InteractionGuid,
			&g.InteractRadius, &g.InteractTolerance, &g.DiscoveryDistance, &g.ChannelDuration, &combat, &single, &los, &cancel, &g.GatherGuid,
			&g.ProfessionGuid, &g.ToolSlot, &g.CertGuid, &g.CertTier, &g.BaseDuration, &g.DurationExpr, &g.DurabilityExpr, &g.RewardGuid); err != nil {
			return nil, err
		}
		g.AllowInCombat, g.SingleUse, g.RequireLOS, g.CancelIfPredFails = combat != 0, single != 0, los != 0, cancel != 0
		out = append(out, g)
	}
	return out, rows.Err()
}

// Cyb.C O A

// Profession is a profession row.
type Profession struct {
	Guid             int64
	Name             string
	Category         string
	LevelingTypeGuid int64
	SkillTreeGuid    int64
}

// Professions reads the 22 CharacterProfessions in guid order (the order the character service stores them in).
func (d *DB) Professions() ([]Profession, error) {
	rows, err := d.sql.Query(`SELECT guid, name, category, leveling_type_guid, skill_tree_guid FROM profession ORDER BY guid`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Profession
	for rows.Next() {
		var p Profession
		if err := rows.Scan(&p.Guid, &p.Name, &p.Category, &p.LevelingTypeGuid, &p.SkillTreeGuid); err != nil {
			return nil, err
		}
		out = append(out, p)
	}
	return out, rows.Err()
}

// CertificationLevel is a certification_level row.
type CertificationLevel struct {
	Guid            int64
	Name            string
	Tier            int
	MinLevel        int
	MaxLevel        int
	HasMaxAllowable bool
	MaxAllowable    int
}

// CertificationLevels reads the 5 certification levels by tier.
func (d *DB) CertificationLevels() ([]CertificationLevel, error) {
	rows, err := d.sql.Query(`SELECT guid, name, tier, min_level, max_level, has_max_allowable, max_allowable FROM certification_level ORDER BY tier`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []CertificationLevel
	for rows.Next() {
		var c CertificationLevel
		var has int
		if err := rows.Scan(&c.Guid, &c.Name, &c.Tier, &c.MinLevel, &c.MaxLevel, &has, &c.MaxAllowable); err != nil {
			return nil, err
		}
		c.HasMaxAllowable = has != 0
		out = append(out, c)
	}
	return out, rows.Err()
}
