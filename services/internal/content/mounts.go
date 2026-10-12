// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
)

// The mounts (milestone "mounts", stage 1; contentc version 11): what a summoned mount is, from the design data.
//
//	mount          the 101 `Mount_*` items (tag Item.Mount.Ground 51, .Flying 46, .Aquatic 4): the Blueprint the item summons (SummonNotifyClassPath), how
//	               the item is used (activation: SummonMount, or CastAbility for the flying ones) and the numbers the item's text names (the speed as a
//	               percentage of the base, 22 of the 51 ground items name none; the Maximum Health percentage of the flyers: 500%, the Drake 1000%)
//	mount_stat     the Mount_Base stat list at the OWNER's level 1 to 50 (a mount's stats are its owner's level: GetCharacterLevel(GetOwner())): MaxHealth
//	               (the Mount_Base_HP curve), HealthRegenerationRating and MitigationRating; a mount's own Health is this times the item's health factor
//	mount_ability  the abilities a rider may use mounted (bUsableWhileMounted: 33) or that dismount the rider first (bAutoDismount: 12), and the
//	               roles of Summon_Mount, MountAbility_Sprint (the dash) and the two dismounts
//	mount_effect   the effects of riding: the road speed effects Road_T0..T4_Speed_Mount (MoveSpeedMult +0.10 .. +0.30), the dash and the effects with
//	               bForceDismount
const MountDDL = `
CREATE TABLE mount (
    item_guid         INTEGER PRIMARY KEY,
    item_name         TEXT NOT NULL,
    kind              TEXT NOT NULL,      -- ground, flying or aquatic (the item's Item.Mount.* tag)
    blueprint_path    TEXT NOT NULL,      -- SummonNotifyClassPath: /Game/GameBlueprints/Mounts/Mount_Grounded_TerrorBird_0.Mount_Grounded_TerrorBird_0_C
    blueprint         TEXT NOT NULL,      -- the package's name: Mount_Grounded_TerrorBird_0 (several items share one Blueprint)
    activation        TEXT NOT NULL,      -- the item's ActivationAction: SummonMount, or CastAbility (the ability is cast_ability_guid: Summon_Mount)
    cast_ability_guid INTEGER NOT NULL,   -- ActivationCastId (0: none)
    level             INTEGER NOT NULL,   -- the "Level N" of the item's text, 0 when it has none
    ground_pct        INTEGER NOT NULL,   -- "165% Ground Speed": 165; 0 when the text names none
    flying_pct        INTEGER NOT NULL,   -- "215% Flying Speed"
    water_pct         INTEGER NOT NULL,   -- "230% Water Speed"
    health_pct        INTEGER NOT NULL,   -- "500% Maximum Health": 500; 100 when the text names none
    mule              INTEGER NOT NULL,   -- a crate mule ("Ground Mule")
    crate_capacity    INTEGER NOT NULL,   -- "Crate Carrying Capacity: 2"
    tradeable         INTEGER NOT NULL,
    description       TEXT NOT NULL       -- the item's text, as the client shows it
);
CREATE INDEX mount_by_blueprint ON mount (blueprint);
-- the Mount_Base stat list evaluated at the owner's level (the stats it sets that depend on the level)
CREATE TABLE mount_stat (
    level     INTEGER NOT NULL,
    stat_guid INTEGER NOT NULL,
    stat_name TEXT NOT NULL,
    value     REAL NOT NULL,
    PRIMARY KEY (level, stat_guid)
) WITHOUT ROWID;
CREATE TABLE mount_ability (
    ability_guid         INTEGER PRIMARY KEY,
    name                 TEXT NOT NULL,
    role                 TEXT NOT NULL,   -- summon, dash, dismount_elevated, dismount_grounded; '' for the others
    usable_while_mounted INTEGER NOT NULL, -- bUsableWhileMounted
    auto_dismount        INTEGER NOT NULL  -- bAutoDismount: the rider dismounts first
);
CREATE TABLE mount_effect (
    effect_guid    INTEGER PRIMARY KEY,
    name           TEXT NOT NULL,
    kind           TEXT NOT NULL,         -- road, dash, force_dismount or crate
    tier           INTEGER NOT NULL,      -- the road's tier T0..T4; -1 for the others
    stat_guid      INTEGER NOT NULL,      -- the stat its (first) mod changes: MoveSpeedMult; 0 for none
    value          REAL NOT NULL,         -- the mod's value: added to the stat; for the dash (relative) the share of the stat it adds
    relative       INTEGER NOT NULL,      -- 1: the mod is value x the current stat (the dash: 0.35 x MoveSpeedMult)
    infinite       INTEGER NOT NULL,
    duration       REAL NOT NULL,         -- seconds; 0 for an infinite effect
    force_dismount INTEGER NOT NULL       -- bForceDismount
);
`

// Mounts, stage 1, known records.
const (
	GuidMoveSpeedMult      = 109228544644729     // StatTypeDef MoveSpeedMult: the stat the road effects and the dash change
	GuidMountBase          = 6064632518022856704 // StatInitializerList Mount_Base
	GuidMountBaseHP        = 6064632377010487296 // StatCurve Mount_Base_HP (the mount's MaxHealth by the owner's level)
	GuidSummonMount        = 108436548376185     // AoCAbility Summon_Mount
	GuidMountAbilitySprint = 6064631555335526877 // AoCAbility MountAbility_Sprint (the dash)
	GuidDismountElevated   = 6064633882790723584 // AoCAbility General_Dismount_Elevated
	GuidDismountGrounded   = 6064633899320803328 // AoCAbility General_Dismount_Grounded
	GuidMountInteraction   = 108725434668664     // InteractionDefinition Mount_Interaction (mounting up)
)

// C:Y:B x CoA

// The kinds of a mount (mount.kind) and the roles of mount_ability and the kinds of mount_effect.
const (
	MountGround  = "ground"
	MountFlying  = "flying"
	MountAquatic = "aquatic"

	MountRoleSummon    = "summon"
	MountRoleDash      = "dash"
	MountRoleDismountE = "dismount_elevated"
	MountRoleDismountG = "dismount_grounded"

	MountEffectRoad          = "road"
	MountEffectDash          = "dash"
	MountEffectForceDismount = "force_dismount"
	MountEffectCrate         = "crate"
)

// Mount is a row of mount.
type Mount struct {
	ItemGuid        int64
	ItemName        string
	Kind            string
	BlueprintPath   string
	Blueprint       string
	Activation      string
	CastAbilityGuid int64
	Level           int
	GroundPct       int
	FlyingPct       int
	WaterPct        int
	HealthPct       int
	Mule            bool
	CrateCapacity   int
	Tradeable       bool
	Description     string
}

// HealthFactor is the multiple of the Mount_Base health the item gives (5 for a flyer, 10 for the Drake of the Canopy).
func (m Mount) HealthFactor() float64 { return float64(m.HealthPct) / 100 }

// MountStat is one row of mount_stat.
type MountStat struct {
	Level    int
	StatGuid int64
	StatName string
	Value    float64
}

// MountAbility is a row of mount_ability.
type MountAbility struct {
	AbilityGuid        int64
	Name               string
	Role               string
	UsableWhileMounted bool
	AutoDismount       bool
}

// MountEffect is a row of mount_effect.
type MountEffect struct {
	EffectGuid    int64
	Name          string
	Kind          string
	Tier          int
	StatGuid      int64
	Value         float64
	Relative      bool
	Infinite      bool
	Duration      float64
	ForceDismount bool
}

// (C | O | A ~ C | Y | B)

func createMounts(db *sql.DB) error {
	if _, err := db.Exec(MountDDL); err != nil {
		return fmt.Errorf("content: create the mount tables: %w", err)
	}
	return nil
}

// Mounts reads every mount, by item name.
func (d *DB) Mounts() ([]Mount, error) {
	rows, err := d.sql.Query(`SELECT item_guid, item_name, kind, blueprint_path, blueprint, activation, cast_ability_guid, level, ground_pct, flying_pct,
		water_pct, health_pct, mule, crate_capacity, tradeable, description FROM mount ORDER BY item_name`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Mount
	for rows.Next() {
		var m Mount
		if err := rows.Scan(&m.ItemGuid, &m.ItemName, &m.Kind, &m.BlueprintPath, &m.Blueprint, &m.Activation, &m.CastAbilityGuid, &m.Level, &m.GroundPct,
			&m.FlyingPct, &m.WaterPct, &m.HealthPct, &m.Mule, &m.CrateCapacity, &m.Tradeable, &m.Description); err != nil {
			return nil, err
		}
		out = append(out, m)
	}
	return out, rows.Err()
}

// MountStats reads the Mount_Base stats at an owner's level (level 1 to 50), by stat guid.
func (d *DB) MountStats(level int) ([]MountStat, error) {
	rows, err := d.sql.Query(`SELECT level, stat_guid, stat_name, value FROM mount_stat WHERE level = ? ORDER BY stat_guid`, level)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []MountStat
	for rows.Next() {
		var s MountStat
		if err := rows.Scan(&s.Level, &s.StatGuid, &s.StatName, &s.Value); err != nil {
			return nil, err
		}
		out = append(out, s)
	}
	return out, rows.Err()
}

// MountAbilities reads the abilities that matter to a rider, by name.
func (d *DB) MountAbilities() ([]MountAbility, error) {
	rows, err := d.sql.Query(`SELECT ability_guid, name, role, usable_while_mounted, auto_dismount FROM mount_ability ORDER BY name`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []MountAbility
	for rows.Next() {
		var a MountAbility
		if err := rows.Scan(&a.AbilityGuid, &a.Name, &a.Role, &a.UsableWhileMounted, &a.AutoDismount); err != nil {
			return nil, err
		}
		out = append(out, a)
	}
	return out, rows.Err()
}

// MountEffects reads the effects of riding, by name.
func (d *DB) MountEffects() ([]MountEffect, error) {
	rows, err := d.sql.Query(`SELECT effect_guid, name, kind, tier, stat_guid, value, relative, infinite, duration, force_dismount FROM mount_effect ORDER BY name`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []MountEffect
	for rows.Next() {
		var e MountEffect
		if err := rows.Scan(&e.EffectGuid, &e.Name, &e.Kind, &e.Tier, &e.StatGuid, &e.Value, &e.Relative, &e.Infinite, &e.Duration, &e.ForceDismount); err != nil {
			return nil, err
		}
		out = append(out, e)
	}
	return out, rows.Err()
}
