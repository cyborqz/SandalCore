// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"errors"
	"fmt"
)

// Plan D1: the typed queries over item_use, item_action, action_response, action_reward, reward_table, reward_item,
// reward_xp, leveling_type, leveling_curve, skill_auto_grant and skill_rank_stat_mod (schema.go says what each holds).

// ItemUse is what using an item does: its activation and the ActionResponses it runs.
type ItemUse struct {
	ItemGuid         int64
	Activation       string // EItemActivationAction without its prefix
	ConsumeOnUse     bool
	Cooldown         float64
	DisabledInCombat bool
	Actions          []int64 // ActionResponse guids, in the record's order
}

// ItemUse returns how the item is used; an error wrapping sql.ErrNoRows for an item that is not used.
func (d *DB) ItemUse(item int64) (ItemUse, error) {
	var u ItemUse
	err := d.sql.QueryRow(`SELECT item_guid, activation, consume_on_use, cooldown, disabled_in_combat FROM item_use WHERE item_guid = ?`, item).
		Scan(&u.ItemGuid, &u.Activation, &u.ConsumeOnUse, &u.Cooldown, &u.DisabledInCombat)
	if err != nil {
		return ItemUse{}, fmt.Errorf("content: item use of %d: %w", item, err)
	}
	rows, err := d.sql.Query(`SELECT action_guid FROM item_action WHERE item_guid = ? ORDER BY ord`, item)
	if err != nil {
		return ItemUse{}, err
	}
	defer rows.Close()
	for rows.Next() {
		var g int64
		if err := rows.Scan(&g); err != nil {
			return ItemUse{}, err
		}
		u.Actions = append(u.Actions, g)
	}
	return u, rows.Err()
}

// ActionResponse is one ActionResponse record an item names.
type ActionResponse struct {
	Guid               int64
	Name               string
	ResponseType       string // EActionResponse without its prefix: GiveItem, SetPrimaryArchetype ...
	ClassGuid          int64  // SetPrimaryArchetype's class (CharacterClasses record), 0 for the others
	ClassName          string
	RemoveUpToQuantity bool
	RewardTables       []int64 // GiveItem's reward tables, in the record's order
}

// -- C:Y:B & C-O-A --

// ActionResponse returns the record; an error wrapping sql.ErrNoRows when there is none.
func (d *DB) ActionResponse(guid int64) (ActionResponse, error) {
	var a ActionResponse
	err := d.sql.QueryRow(`SELECT guid, name, response_type, class_guid, class_name, remove_up_to_quantity FROM action_response WHERE guid = ?`, guid).
		Scan(&a.Guid, &a.Name, &a.ResponseType, &a.ClassGuid, &a.ClassName, &a.RemoveUpToQuantity)
	if err != nil {
		return ActionResponse{}, fmt.Errorf("content: action response %d: %w", guid, err)
	}
	rows, err := d.sql.Query(`SELECT reward_table_guid FROM action_reward WHERE action_guid = ? ORDER BY ord`, guid)
	if err != nil {
		return ActionResponse{}, err
	}
	defer rows.Close()
	for rows.Next() {
		var g int64
		if err := rows.Scan(&g); err != nil {
			return ActionResponse{}, err
		}
		a.RewardTables = append(a.RewardTables, g)
	}
	return a, rows.Err()
}

// RewardItem is one ItemRewards entry of a reward table.
type RewardItem struct {
	ItemGuid      int64
	VariationGuid int64
	Quantity      int
}

// RewardXP is one ExperienceRewards entry: the level scrolls' formula resolved to its parameters (schema.go reward_xp).
type RewardXP struct {
	ExperienceType string // EExperienceContextType without its prefix: Adventuring, Weapon ...
	TargetLevel    int    // 0: the expression is not the level scrolls' shape (the reward is refused)
	Margin         float64
	CurveGuid      int64
	MultStatGuid   int64
	Expression     string
}

// RewardTable is a modelled reward table: what it grants.
type RewardTable struct {
	Guid       int64
	Name       string
	Predicate  string
	LevelBelow int // the predicate 'GetPlayerLevel() < N' as N; 0 none; -1 a predicate contentc does not model
	Modelled   bool
	Items      []RewardItem
	Experience []RewardXP
}

// RewardTable returns the table; an error wrapping sql.ErrNoRows when there is none.
func (d *DB) RewardTable(guid int64) (RewardTable, error) {
	var t RewardTable
	err := d.sql.QueryRow(`SELECT guid, name, predicate, level_below, modelled FROM reward_table WHERE guid = ?`, guid).
		Scan(&t.Guid, &t.Name, &t.Predicate, &t.LevelBelow, &t.Modelled)
	if err != nil {
		return RewardTable{}, fmt.Errorf("content: reward table %d: %w", guid, err)
	}
	rows, err := d.sql.Query(`SELECT item_guid, variation_guid, quantity FROM reward_item WHERE table_guid = ? ORDER BY ord`, guid)
	if err != nil {
		return RewardTable{}, err
	}
	for rows.Next() {
		var it RewardItem
		if err := rows.Scan(&it.ItemGuid, &it.VariationGuid, &it.Quantity); err != nil {
			rows.Close()
			return RewardTable{}, err
		}
		t.Items = append(t.Items, it)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return RewardTable{}, err
	}
	xrows, err := d.sql.Query(`SELECT experience_type, target_level, margin, curve_guid, mult_stat_guid, expression FROM reward_xp WHERE table_guid = ? ORDER BY ord`, guid)
	if err != nil {
		return RewardTable{}, err
	}
	defer xrows.Close()
	for xrows.Next() {
		var x RewardXP
		if err := xrows.Scan(&x.ExperienceType, &x.TargetLevel, &x.Margin, &x.CurveGuid, &x.MultStatGuid, &x.Expression); err != nil {
			return RewardTable{}, err
		}
		t.Experience = append(t.Experience, x)
	}
	return t, xrows.Err()
}

// LevelingType is a LevelingTypeDef with its level curve.
type LevelingType struct {
	Guid           int64
	Name           string
	ExperienceType string
	LevelCap       int // LevelCapOverride; 0 none (the curve's last level)
	UsesDebt       bool
	XPMultStatGuid int64
	Curve          []LevelXP // ascending by level
}

// LevelXP is one key of a level curve: the XP total at which the level is reached.
type LevelXP struct {
	Level int
	XP    int64
}

// LevelingTypeByName returns the leveling type named ("Adventuring"); an error wrapping sql.ErrNoRows when there is none.
func (d *DB) LevelingTypeByName(name string) (LevelingType, error) {
	var t LevelingType
	err := d.sql.QueryRow(`SELECT guid, name, experience_type, level_cap, uses_debt, xp_mult_stat_guid FROM leveling_type WHERE name = ?`, name).
		Scan(&t.Guid, &t.Name, &t.ExperienceType, &t.LevelCap, &t.UsesDebt, &t.XPMultStatGuid)
	if err != nil {
		return LevelingType{}, fmt.Errorf("content: leveling type %q: %w", name, err)
	}
	rows, err := d.sql.Query(`SELECT level, xp FROM leveling_curve WHERE type_guid = ? ORDER BY level`, t.Guid)
	if err != nil {
		return LevelingType{}, err
	}
	defer rows.Close()
	for rows.Next() {
		var k LevelXP
		if err := rows.Scan(&k.Level, &k.XP); err != nil {
			return LevelingType{}, err
		}
		t.Curve = append(t.Curve, k)
	}
	return t, rows.Err()
}

// AutoGrant is a SkillTreeNode a level grants by itself.
type AutoGrant struct {
	TreeGuid         int64
	TreeName         string
	LevelingTypeGuid int64
	NodeGuid         int64
	NodeName         string
	LevelRequirement int
	SkillGuid        int64
	SkillName        string
	RankGuid         int64
	EffectGuid       int64
	Mods             []RankStatMod // the constant stat mods of the rank's passive effect
}

// RankStatMod is a constant stat mod of a rank's passive effect.
type RankStatMod struct {
	StatGuid int64
	StatName string
	Value    float64
}

// ~ C | O | A & C | Y | B ~

// AutoGrants returns the nodes of the trees of a leveling type that a level grants, by level requirement then node guid.
func (d *DB) AutoGrants(levelingType int64) ([]AutoGrant, error) {
	rows, err := d.sql.Query(`SELECT tree_guid, tree_name, leveling_type_guid, node_guid, node_name, level_requirement, skill_guid, skill_name,
		rank_guid, effect_guid FROM skill_auto_grant WHERE leveling_type_guid = ? ORDER BY level_requirement, node_guid`, levelingType)
	if err != nil {
		return nil, err
	}
	var out []AutoGrant
	for rows.Next() {
		var g AutoGrant
		if err := rows.Scan(&g.TreeGuid, &g.TreeName, &g.LevelingTypeGuid, &g.NodeGuid, &g.NodeName, &g.LevelRequirement, &g.SkillGuid, &g.SkillName,
			&g.RankGuid, &g.EffectGuid); err != nil {
			rows.Close()
			return nil, err
		}
		out = append(out, g)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}
	for i := range out {
		mrows, err := d.sql.Query(`SELECT stat_guid, stat_name, value FROM skill_rank_stat_mod WHERE rank_guid = ? ORDER BY ord`, out[i].RankGuid)
		if err != nil {
			return nil, err
		}
		for mrows.Next() {
			var m RankStatMod
			if err := mrows.Scan(&m.StatGuid, &m.StatName, &m.Value); err != nil {
				mrows.Close()
				return nil, err
			}
			out[i].Mods = append(out[i].Mods, m)
		}
		mrows.Close()
		if err := mrows.Err(); err != nil {
			return nil, err
		}
	}
	return out, nil
}

// ItemByName returns the item named exactly `name`; an error wrapping sql.ErrNoRows when there is none.
func (d *DB) ItemByName(name string) (Item, error) {
	var guid int64
	if err := d.sql.QueryRow(`SELECT guid FROM item WHERE name = ?`, name).Scan(&guid); err != nil {
		return Item{}, fmt.Errorf("content: item %q: %w", name, err)
	}
	return d.Item(guid)
}

// ItemsNamedLike returns the names and guids of the items whose name matches the SQL LIKE pattern (a '|' escapes % and
// _), by name.
func (d *DB) ItemsNamedLike(pattern string) ([]NamedGuid, error) {
	return d.namedGuids(`SELECT 0, guid, name FROM item WHERE name LIKE ? ESCAPE '|' ORDER BY name`, pattern)
}

// IsNoRow reports whether err says the row does not exist (the typed queries wrap sql.ErrNoRows).
func IsNoRow(err error) bool { return errors.Is(err, sql.ErrNoRows) }
