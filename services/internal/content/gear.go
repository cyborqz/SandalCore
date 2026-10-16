// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"errors"
	"fmt"
	"strconv"
)

// Plan B3: what an item adds to a character's stats, and the compiled stat program, as schema.go lays them out (contentc
// version 4). worldd reads the same rows in C++ (server/worldd/content/content_db.cpp); these readers are the services'
// and the tests'.

// MetaContentcVersion is the meta key that says which contentc wrote the file.
const MetaContentcVersion = "contentc.version"

// MinContentcVersion is the oldest contentc whose content.db this checkout reads: version 4 has the stat program and the
// item stat tables (plan B3), version 5 the item uses, reward tables, level curves and auto-granted ranks (plan D1), version 6 the ability tables and the stat bundle's
// format 2 (effect state and expression programs, plan D2), version 7 the NPC population chain and the vendors (plan E1, stage 1), version 8 the routes' waypoints and the instances' patrol settings (stage 2), version 9 the combat tables and the spawn filter's records (stage 3), version 10 the reward tables (loot_table, stage 3 part 2), version 11 the mounts (mount, mount_stat, mount_ability and mount_effect, the mounts milestone), version 12 the city nodes (city_node, city_zoi, the node tables of nodes.go: Nodes 1, stage 1), version 13 the gathering tables (gather_def, gather_tool, item_variation, profession, certification_level and the others of gather.go), the profession level-up curves and the host functions 180-184 (Gathering stage 2), version 14 the items' inventory dimension and resource flag and the resource bags' grids (item.inv_w, inv_h, resource; resource_bag, resource_bag_cell: Gathering stage 3, "Resource storage"), version 15 the foliage defs and states and every gathering interaction (foliage_def, foliage_state, gather_interaction, gather_interaction_def: Gathering stage 3, "Foliage gathering"), version 16 the items' display name (item.display_name, the record's ItemName: Gathering stage 3, "the pick-up popup").
const MinContentcVersion = 15

// RebuildHint is the command that makes a content.db this checkout reads.
const RebuildHint = "rebuild it with this checkout's contentc: cd services && go run ./cmd/contentc"

// RequireVersion refuses a content.db that contentc wrote before MinContentcVersion, or that has no stat program, with a
// message that says what to do. The services call it so that an old file fails at start and not when a stat is first wanted.
func (d *DB) RequireVersion() error {
	meta, err := d.Meta()
	if err != nil {
		return err
	}
	v, ok := meta[MetaContentcVersion]
	n, perr := strconv.Atoi(v)
	if !ok || perr != nil {
		return fmt.Errorf("content: the file has no usable %s in meta (%q): it was not written by contentc; %s", MetaContentcVersion, v, RebuildHint)
	}
	if n < MinContentcVersion {
		return fmt.Errorf("content: the file was built by contentc version %d, this checkout needs version %d or later (the foliage defs and states and the gathering interactions, Gathering stage 3): %s",
			n, MinContentcVersion, RebuildHint)
	}
	var rows int
	if err := d.sql.QueryRow(`SELECT COUNT(*) FROM stat_program`).Scan(&rows); err != nil {
		return fmt.Errorf("content: the file has no stat program table (%v): %s", err, RebuildHint)
	}
	if rows != 1 {
		return fmt.Errorf("content: the file's stat_program table has %d rows, want 1: %s", rows, RebuildHint)
	}
	return nil
}

// C O A with Cyb

// StatProgram is the compiled stat formula graph as the file holds it.
type StatProgram struct {
	Version int    // the bundle format's version
	Mode    string // float32, double or single
	Stats   int
	Classes int
	CRC32   uint32
	Data    []byte
}

// StatProgram reads the one stat_program row.
func (d *DB) StatProgram() (StatProgram, error) {
	var p StatProgram
	var crc int64
	err := d.sql.QueryRow(`SELECT version, mode, stats, classes, crc32, data FROM stat_program WHERE id = 1`).
		Scan(&p.Version, &p.Mode, &p.Stats, &p.Classes, &crc, &p.Data)
	if errors.Is(err, sql.ErrNoRows) {
		return StatProgram{}, errors.New("content: the file has no stat program")
	}
	if err != nil {
		return StatProgram{}, err
	}
	p.CRC32 = uint32(crc)
	return p, nil
}

// Rarities are the seven bands of an item's quality, in the order of the tables' columns.
var Rarities = [7]string{"Common", "Uncommon", "Rare", "Heroic", "Epic", "Legendary", "Artifact"}

// QualityBand is the range of Quality of one rarity.
type QualityBand struct{ X, Y float64 }

// QualityCurve returns the seven bands of a quality curve, by rarity.
func (d *DB) QualityCurve(guid int64) ([7]QualityBand, error) {
	var out [7]QualityBand
	rows, err := d.sql.Query(`SELECT ord, x, y FROM quality_curve WHERE guid = ? ORDER BY ord`, guid)
	if err != nil {
		return out, err
	}
	defer rows.Close()
	n := 0
	for rows.Next() {
		var ord int
		var b QualityBand
		if err := rows.Scan(&ord, &b.X, &b.Y); err != nil {
			return out, err
		}
		if ord != n || ord > 6 {
			return out, fmt.Errorf("content: quality curve %d has rarity %d where %d is next", guid, ord, n)
		}
		out[ord] = b
		n++
	}
	if err := rows.Err(); err != nil {
		return out, err
	}
	if n != 7 {
		return out, fmt.Errorf("content: quality curve %d has %d rarities, want 7", guid, n)
	}
	return out, nil
}

// RarityValue is the Min and Max of a stat at one rarity.
type RarityValue struct{ Min, Max float64 }

// StatBlockStat is one stat of an item stat block.
type StatBlockStat struct {
	Stat   int64
	Values [7]RarityValue
}

// StatBlock returns the stats of an item stat block, by stat Guid.
func (d *DB) StatBlock(guid int64) ([]StatBlockStat, error) {
	rows, err := d.sql.Query(`SELECT stat_guid, common_min, common_max, uncommon_min, uncommon_max, rare_min, rare_max, heroic_min, heroic_max,
		epic_min, epic_max, legendary_min, legendary_max, artifact_min, artifact_max FROM stat_block_stat WHERE block_guid = ? ORDER BY stat_guid`, guid)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []StatBlockStat
	for rows.Next() {
		var s StatBlockStat
		if err := rows.Scan(&s.Stat, &s.Values[0].Min, &s.Values[0].Max, &s.Values[1].Min, &s.Values[1].Max, &s.Values[2].Min, &s.Values[2].Max,
			&s.Values[3].Min, &s.Values[3].Max, &s.Values[4].Min, &s.Values[4].Max, &s.Values[5].Min, &s.Values[5].Max, &s.Values[6].Min, &s.Values[6].Max); err != nil {
			return nil, err
		}
		out = append(out, s)
	}
	return out, rows.Err()
}

// ItemSets returns the SetBonus Guids an item belongs to, in the record's order.
func (d *DB) ItemSets(item int64) ([]int64, error) {
	rows, err := d.sql.Query(`SELECT set_guid FROM item_set WHERE item_guid = ? ORDER BY ord`, item)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []int64
	for rows.Next() {
		var g int64
		if err := rows.Scan(&g); err != nil {
			return nil, err
		}
		out = append(out, g)
	}
	return out, rows.Err()
}

// ItemTags returns the gameplay tags of an item, in the record's order.
func (d *DB) ItemTags(item int64) ([]string, error) {
	rows, err := d.sql.Query(`SELECT tag FROM item_tag WHERE item_guid = ? ORDER BY ord`, item)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []string
	for rows.Next() {
		var t string
		if err := rows.Scan(&t); err != nil {
			return nil, err
		}
		out = append(out, t)
	}
	return out, rows.Err()
}

// CoA + C | Y | B

// SetBonusEntry is one bonus of a tier: the stat and its value at each rarity (Has[i] false: the record gives none).
type SetBonusEntry struct {
	Stat   int64
	Values [7]float64
	Has    [7]bool
}

// SetBonusTier is the bonuses that apply once Pieces of the set are worn.
type SetBonusTier struct {
	Pieces  int
	Entries []SetBonusEntry
}

// SetBonus returns the tiers of a set bonus in the record's order.
func (d *DB) SetBonus(guid int64) ([]SetBonusTier, error) {
	rows, err := d.sql.Query(`SELECT tier_ord, pieces, stat_guid, common, uncommon, rare, heroic, epic, legendary, artifact
		FROM set_bonus WHERE set_guid = ? ORDER BY tier_ord, bonus_ord`, guid)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []SetBonusTier
	tierOrd := -1
	for rows.Next() {
		var ord, pieces int
		var e SetBonusEntry
		var v [7]sql.NullFloat64
		if err := rows.Scan(&ord, &pieces, &e.Stat, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]); err != nil {
			return nil, err
		}
		for i := range v {
			e.Values[i], e.Has[i] = v[i].Float64, v[i].Valid
		}
		if ord < 0 {
			return nil, fmt.Errorf("content.db's set %d has a negative tier order %d", guid, ord)
		}
		if len(out) == 0 || ord != tierOrd {
			out = append(out, SetBonusTier{Pieces: pieces})
			tierOrd = ord
		}
		out[len(out)-1].Entries = append(out[len(out)-1].Entries, e)
	}
	return out, rows.Err()
}
