// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"fmt"
)

// Plan D1, stage 2: the talents (schema.go: skill_tree, skill_node, skill_node_require, skill_node_exclusive, skill_rank,
// skill_rank_cost, skill_point_type, leveling_point, item_weapon). The services do not read them; contentc's tests do, and so
// does anyone who wants to look at what worldd's talents catalog reads.

// The kinds of a skill tree.
const (
	TreeClass      = "class"
	TreeUniversal  = "universal"
	TreeWeapon     = "weapon"
	TreeProfession = "profession"
	TreeOther      = "other"
)

// SkillTree is a row of skill_tree.
type SkillTree struct {
	Guid             int64
	Name             string
	Kind             string
	LevelingTypeGuid int64
	ClassGuid        int64 // a class tree's class
	WeaponLineGuid   int64 // a weapon tree's WeaponSkillLines record
}

// SkillTreeByName returns the tree named; an error wrapping sql.ErrNoRows when there is none.
func (d *DB) SkillTreeByName(name string) (SkillTree, error) {
	var t SkillTree
	err := d.sql.QueryRow(`SELECT guid, name, kind, leveling_type_guid, class_guid, weapon_line_guid FROM skill_tree WHERE name = ?`, name).
		Scan(&t.Guid, &t.Name, &t.Kind, &t.LevelingTypeGuid, &t.ClassGuid, &t.WeaponLineGuid)
	if err != nil {
		return SkillTree{}, fmt.Errorf("content: skill tree %q: %w", name, err)
	}
	return t, nil
}

// SkillTrees returns every tree by guid.
func (d *DB) SkillTrees() ([]SkillTree, error) {
	rows, err := d.sql.Query(`SELECT guid, name, kind, leveling_type_guid, class_guid, weapon_line_guid FROM skill_tree ORDER BY guid`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []SkillTree
	for rows.Next() {
		var t SkillTree
		if err := rows.Scan(&t.Guid, &t.Name, &t.Kind, &t.LevelingTypeGuid, &t.ClassGuid, &t.WeaponLineGuid); err != nil {
			return nil, err
		}
		out = append(out, t)
	}
	return out, rows.Err()
}

// SkillNode is a row of skill_node with its requirements (the skills that must be owned) and exclusions.
type SkillNode struct {
	TreeGuid         int64
	NodeGuid         int64
	Ord              int // the node's place in the tree's Skills list
	Name             string
	SkillGuid        int64
	SkillName        string
	LevelRequirement int
	PointRequirement int
	AutoGranted      bool
	AutoHotbar       bool
	AllRequired      bool
	Opt              int     // 0 for a plain node; for a slot node the option's place in the slot (the node is listed once per option)
	SlotGuid         int64   // the SkillTreeSlot a slot node offers its skills from (0: a plain node)
	Requires         []int64 // skill guids; 0: a requirement that names nothing (met)
	RequiresNodes    []int64 // slot nodes (of the same tree) whose options meet a requirement: any option owned
	Exclusive        []int64 // skill guids
}

// [C_Y_B x C.O.A]

// SkillNodes returns a tree's nodes in the tree's order.
func (d *DB) SkillNodes(tree int64) ([]SkillNode, error) {
	rows, err := d.sql.Query(`SELECT tree_guid, node_guid, ord, name, skill_guid, skill_name, level_requirement, point_requirement, auto_granted, auto_hotbar,
		all_required, opt, slot_guid FROM skill_node WHERE tree_guid = ? ORDER BY ord, opt`, tree)
	if err != nil {
		return nil, err
	}
	var out []SkillNode
	for rows.Next() {
		var n SkillNode
		if err := rows.Scan(&n.TreeGuid, &n.NodeGuid, &n.Ord, &n.Name, &n.SkillGuid, &n.SkillName, &n.LevelRequirement, &n.PointRequirement, &n.AutoGranted,
			&n.AutoHotbar, &n.AllRequired, &n.Opt, &n.SlotGuid); err != nil {
			rows.Close()
			return nil, err
		}
		out = append(out, n)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}
	for i := range out {
		r, err := d.sql.Query(`SELECT skill_guid, slot_node_guid FROM skill_node_require WHERE tree_guid = ? AND node_guid = ? ORDER BY ord`, tree, out[i].NodeGuid)
		if err != nil {
			return nil, err
		}
		for r.Next() {
			var g, slot int64
			if err := r.Scan(&g, &slot); err != nil {
				r.Close()
				return nil, err
			}
			if slot != 0 {
				out[i].RequiresNodes = append(out[i].RequiresNodes, slot)
			} else {
				out[i].Requires = append(out[i].Requires, g)
			}
		}
		r.Close()
		if err := r.Err(); err != nil {
			return nil, err
		}
		x, err := d.sql.Query(`SELECT skill_guid FROM skill_node_exclusive WHERE tree_guid = ? AND node_guid = ? ORDER BY ord`, tree, out[i].NodeGuid)
		if err != nil {
			return nil, err
		}
		for x.Next() {
			var g int64
			if err := x.Scan(&g); err != nil {
				x.Close()
				return nil, err
			}
			out[i].Exclusive = append(out[i].Exclusive, g)
		}
		x.Close()
		if err := x.Err(); err != nil {
			return nil, err
		}
	}
	return out, nil
}

// RankCost is what a rank costs in points of one type.
type RankCost struct {
	PointTypeGuid int64
	Quantity      int64
}

// SkillRank is a row of skill_rank with its costs.
type SkillRank struct {
	SkillGuid      int64
	Ord            int
	RankGuid       int64
	Name           string
	AbilityGuid    int64
	AbilityName    string
	EffectGuid     int64
	EffectName     string
	EffectInfinite bool
	EffectDuration float64 // seconds; 0 with an infinite effect
	Costs          []RankCost
}

// SkillRanks returns a skill's ranks in order.
func (d *DB) SkillRanks(skill int64) ([]SkillRank, error) {
	rows, err := d.sql.Query(`SELECT skill_guid, ord, rank_guid, name, ability_guid, ability_name, effect_guid, effect_name, effect_infinite, effect_duration
		FROM skill_rank WHERE skill_guid = ? ORDER BY ord`, skill)
	if err != nil {
		return nil, err
	}
	var out []SkillRank
	for rows.Next() {
		var r SkillRank
		if err := rows.Scan(&r.SkillGuid, &r.Ord, &r.RankGuid, &r.Name, &r.AbilityGuid, &r.AbilityName, &r.EffectGuid, &r.EffectName, &r.EffectInfinite, &r.EffectDuration); err != nil {
			rows.Close()
			return nil, err
		}
		out = append(out, r)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}
	for i := range out {
		r, err := d.sql.Query(`SELECT point_type_guid, quantity FROM skill_rank_cost WHERE rank_guid = ? ORDER BY ord`, out[i].RankGuid)
		if err != nil {
			return nil, err
		}
		for r.Next() {
			var c RankCost
			if err := r.Scan(&c.PointTypeGuid, &c.Quantity); err != nil {
				r.Close()
				return nil, err
			}
			out[i].Costs = append(out[i].Costs, c)
		}
		r.Close()
		if err := r.Err(); err != nil {
			return nil, err
		}
	}
	return out, nil
}

// RankStatMods returns the constant stat mods of a rank's infinite effect.
func (d *DB) RankStatMods(rank int64) ([]RankStatMod, error) {
	rows, err := d.sql.Query(`SELECT stat_guid, stat_name, value FROM skill_rank_stat_mod WHERE rank_guid = ? ORDER BY ord`, rank)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []RankStatMod
	for rows.Next() {
		var m RankStatMod
		if err := rows.Scan(&m.StatGuid, &m.StatName, &m.Value); err != nil {
			return nil, err
		}
		out = append(out, m)
	}
	return out, rows.Err()
}

// C-O-A captures, C.Y.B code

// SkillPointTypes returns the point types' guids by name.
func (d *DB) SkillPointTypes() (map[string]int64, error) {
	rows, err := d.sql.Query(`SELECT guid, name FROM skill_point_type`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[string]int64{}
	for rows.Next() {
		var g int64
		var n string
		if err := rows.Scan(&g, &n); err != nil {
			return nil, err
		}
		out[n] = g
	}
	return out, rows.Err()
}

// LevelingPoints returns what reaching `level` of a leveling type gives, by point type guid.
func (d *DB) LevelingPoints(levelingType int64, level int) (map[int64]int64, error) {
	rows, err := d.sql.Query(`SELECT point_type_guid, quantity FROM leveling_point WHERE type_guid = ? AND level = ?`, levelingType, level)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[int64]int64{}
	for rows.Next() {
		var p, q int64
		if err := rows.Scan(&p, &q); err != nil {
			return nil, err
		}
		out[p] = q
	}
	return out, rows.Err()
}

// ItemWeapon is a row of item_weapon.
type ItemWeapon struct {
	ItemGuid          int64
	AttackAbilityGuid int64
	SkillLineGuid     int64
	SkillLineName     string
}

// ItemWeapon returns an item's combo ability and weapon line; an error wrapping sql.ErrNoRows when it names none.
func (d *DB) ItemWeapon(item int64) (ItemWeapon, error) {
	var w ItemWeapon
	err := d.sql.QueryRow(`SELECT item_guid, attack_ability_guid, skill_line_guid, skill_line_name FROM item_weapon WHERE item_guid = ?`, item).
		Scan(&w.ItemGuid, &w.AttackAbilityGuid, &w.SkillLineGuid, &w.SkillLineName)
	if err != nil {
		return ItemWeapon{}, fmt.Errorf("content: weapon item %d: %w", item, err)
	}
	return w, nil
}
