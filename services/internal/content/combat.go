// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
)

// The combat configuration (plan E1, stage 3), from the client's DefaultCombat.ini ([/Script/GameSystemsPlugin.CombatConfig] GameSettings.Stats): the stats
// the damage formulas read (the DamageProfile: the Power, Mitigation, Penetration and Critical stats of the physical and the magical aspect and one set
// per element) and the numbers of the combat rules (the in-combat timer, the global damage and healing multipliers, the distances).
const CombatDDL = `
-- DamageProfile's stat references by key ("PowerMult", "Physical.Damage", "Magical.CriticalPower" ...).
CREATE TABLE combat_profile (
    key       TEXT PRIMARY KEY,
    stat_guid INTEGER NOT NULL
) WITHOUT ROWID;
-- One row per element tag of the profile (Element.Physical.Slash ...): the stats of its damage, multipliers and mitigation.
CREATE TABLE combat_element (
    tag             TEXT PRIMARY KEY,
    damage          INTEGER NOT NULL,
    damage_mult     INTEGER NOT NULL,
    damage_add_mult INTEGER NOT NULL,
    mitigation      INTEGER NOT NULL
) WITHOUT ROWID;
-- Who fights whom: the relationship records (Hostile, Friendly, Neutral ...: CombatPermissions 1 hostile, 2 friendly, 3 neutral) and every alignment's
-- relationship to the other alignments (CharacterAlignment.RelationshipsIds).
CREATE TABLE relationship (
    guid        INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    permissions INTEGER NOT NULL
);
CREATE TABLE alignment_relation (
    alignment    INTEGER NOT NULL,
    other        INTEGER NOT NULL,
    relationship INTEGER NOT NULL,
    PRIMARY KEY (alignment, other)
) WITHOUT ROWID;
-- The combat rules' numbers by key (in_combat_timer, global_damage_multiplier ...); source says where each comes from.
CREATE TABLE combat_config (
    key    TEXT PRIMARY KEY,
    value  REAL NOT NULL,
    source TEXT NOT NULL
) WITHOUT ROWID;
`

// The keys of combat_profile, in the order of the damage functions' reads.
var CombatProfileKeys = []string{
	"PowerMult", "PowerAddMult", "DamagePowerMult", "DamagePowerAddMult", "Corruption",
	"Physical.Damage", "Physical.DamageMult", "Physical.DamageAddMult", "Physical.Mitigation", "Physical.Penetration",
	"Physical.CriticalPower", "Physical.CriticalMitigation", "Physical.BlockMitigation",
	"Magical.Damage", "Magical.DamageMult", "Magical.DamageAddMult", "Magical.Mitigation", "Magical.Penetration",
	"Magical.CriticalPower", "Magical.CriticalMitigation", "Magical.BlockMitigation",
}

// built with C | Y | B and CoA

// The keys of combat_config.
const (
	CombatInCombatTimer     = "in_combat_timer"           // seconds after the last hit dealt or taken that a combatant stays in combat
	CombatGlobalDamageMult  = "global_damage_multiplier"  // the DamageMod equation's GlobalMod
	CombatGlobalHealingMult = "global_healing_multiplier" //
	CombatOutOfRange        = "out_of_range_distance"     // cm
	CombatActivationLeeway  = "server_activation_leeway"  // cm: how far past its range the server lets a cast start
	CombatThirdPartyHit     = "third_party_hit_distance"  // cm
	CombatMaxHitsPerRPC     = "max_hits_per_rpc"
	CombatProjectileHitDist = "projectile_hit_distance" // cm
	CombatSoftLockMaxTarget = "max_target_distance"     // cm
	CombatStatRegenTime     = "stat_regen_time"         // seconds between the stat regeneration ticks
)

// The CharacterRelationship CombatPermissions values.
const (
	PermissionHostile  = 1
	PermissionFriendly = 2
	PermissionNeutral  = 3
)

// The CharacterAlignment records a player carries (Player) and the one that makes an NPC an enemy of the players (NPC_PlayerEnemy).
const (
	GuidAlignmentPlayer         = 6064629444695359488
	GuidAlignmentNPCPlayerEnemy = 6064629454762803230
)

// CombatElement is the stats of an element of the damage profile.
type CombatElement struct {
	Tag                                           string
	Damage, DamageMult, DamageAddMult, Mitigation int64
}

// Combat is the combat configuration.
type Combat struct {
	Profile  map[string]int64
	Elements []CombatElement
	Config   map[string]float64
	// Permissions is the CombatPermissions of each relationship record, and Relation[a][b] the relationship record alignment a has to alignment b.
	Permissions map[int64]int
	Relation    map[int64]map[int64]int64
}

func createCombat(db *sql.DB) error {
	if _, err := db.Exec(CombatDDL); err != nil {
		return fmt.Errorf("content: create the combat tables: %w", err)
	}
	return nil
}

// Combat reads the combat configuration (empty maps when the file has none).
func (d *DB) Combat() (*Combat, error) {
	c := &Combat{Profile: map[string]int64{}, Config: map[string]float64{}, Permissions: map[int64]int{}, Relation: map[int64]map[int64]int64{}}
	rows, err := d.sql.Query(`SELECT key, stat_guid FROM combat_profile`)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var k string
		var g int64
		if err := rows.Scan(&k, &g); err != nil {
			rows.Close()
			return nil, err
		}
		c.Profile[k] = g
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, err
	}
	rows.Close()
	rows, err = d.sql.Query(`SELECT tag, damage, damage_mult, damage_add_mult, mitigation FROM combat_element ORDER BY tag`)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var e CombatElement
		if err := rows.Scan(&e.Tag, &e.Damage, &e.DamageMult, &e.DamageAddMult, &e.Mitigation); err != nil {
			rows.Close()
			return nil, err
		}
		c.Elements = append(c.Elements, e)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, err
	}
	rows.Close()
	rows, err = d.sql.Query(`SELECT guid, permissions FROM relationship`)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var g int64
		var p int
		if err := rows.Scan(&g, &p); err != nil {
			rows.Close()
			return nil, err
		}
		c.Permissions[g] = p
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, err
	}
	rows.Close()
	rows, err = d.sql.Query(`SELECT alignment, other, relationship FROM alignment_relation`)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var a, o, r int64
		if err := rows.Scan(&a, &o, &r); err != nil {
			rows.Close()
			return nil, err
		}
		if c.Relation[a] == nil {
			c.Relation[a] = map[int64]int64{}
		}
		c.Relation[a][o] = r
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, err
	}
	rows.Close()
	rows, err = d.sql.Query(`SELECT key, value FROM combat_config`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	for rows.Next() {
		var k string
		var v float64
		if err := rows.Scan(&k, &v); err != nil {
			return nil, err
		}
		c.Config[k] = v
	}
	return c, rows.Err()
}
