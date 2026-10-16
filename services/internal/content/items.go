// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"errors"
	"fmt"
)

// Item is a row of the item table with its slots: what the character service and worldd need of an item (plan B2).
type Item struct {
	Guid                  int64
	Name                  string
	Level                 int
	CanEquip              bool // the record's bCanEquip: NOT what the client wears (see schema.go)
	OccupiesAllEquipSlots bool // a two-handed item: worn, it takes every slot of EquipSlots
	DefaultQuality        int
	MaxStackSize          int
	StatBlockGuid         int64   // 0: none
	AppearanceGuid        int64   // 0: none
	MaxDurability         float64 // at DefaultQuality; 0 for an item with none
	QualityCurveGuid      int64   // plan B3: 0: none
	InvW, InvH            int     // Gathering stage 3 (version 14): the cells the item takes in a resource container's grid (1 x 1 for most)
	Resource              bool    // its GameplayTags hold a child of item.resource: it is stored in a Resource container, never in the Backpack
	DisplayName           string  // Gathering stage 3 (version 16): the record's ItemName source text ("Coconut Wood"); '' where the record has none
	EquipSlots            []int   // EEquipSlot values, in the record's order; empty for an item that is not worn
}

// Item returns the item with the record Guid; an error for a Guid that is none.
func (d *DB) Item(guid int64) (Item, error) {
	var it Item
	err := d.sql.QueryRow(`SELECT guid, name, level, can_equip, occupies_all_equip_slots, default_quality, max_stack_size,
		stat_block_guid, appearance_guid, max_durability, quality_curve_guid, inv_w, inv_h, resource, display_name FROM item WHERE guid = ?`, guid).
		Scan(&it.Guid, &it.Name, &it.Level, &it.CanEquip, &it.OccupiesAllEquipSlots, &it.DefaultQuality, &it.MaxStackSize,
			&it.StatBlockGuid, &it.AppearanceGuid, &it.MaxDurability, &it.QualityCurveGuid, &it.InvW, &it.InvH, &it.Resource, &it.DisplayName)
	if errors.Is(err, sql.ErrNoRows) {
		return Item{}, fmt.Errorf("content: no item %d", guid)
	}
	if err != nil {
		return Item{}, err
	}
	rows, err := d.sql.Query(`SELECT slot FROM item_equip_slot WHERE item_guid = ? ORDER BY ord`, guid)
	if err != nil {
		return Item{}, err
	}
	defer rows.Close()
	for rows.Next() {
		var s int
		if err := rows.Scan(&s); err != nil {
			return Item{}, err
		}
		it.EquipSlots = append(it.EquipSlots, s)
	}
	return it, rows.Err()
}

// -- C | O | A . Cyb --

// KitItem is one item of a class's starting kit.
type KitItem struct {
	Container     string // KitEquipment or KitBackpack
	Ord           int    // the position in the record's list
	Slot          int    // the EEquipSlot of an equipped item; the position of a stored one
	ItemGuid      int64
	VariationGuid int64
	Quantity      int
}

// StartingKit returns the items a new character of the class (by name: "Bard") starts with, the equipped ones first,
// each group in the record's order. An error for a class that has no kit.
func (d *DB) StartingKit(className string) ([]KitItem, error) {
	rows, err := d.sql.Query(`SELECT container, ord, slot, item_guid, variation_guid, quantity FROM starting_equipment
		WHERE class_name = ? ORDER BY CASE container WHEN 'Equipment' THEN 0 ELSE 1 END, ord`, className)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []KitItem
	for rows.Next() {
		var k KitItem
		if err := rows.Scan(&k.Container, &k.Ord, &k.Slot, &k.ItemGuid, &k.VariationGuid, &k.Quantity); err != nil {
			return nil, err
		}
		out = append(out, k)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("content: no starting kit for class %q", className)
	}
	return out, nil
}

// EquipSlotNames returns the name of each EEquipSlot value ("Chest" for 2).
func (d *DB) EquipSlotNames() (map[int]string, error) {
	rows, err := d.sql.Query(`SELECT slot, name FROM equip_slot`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[int]string{}
	for rows.Next() {
		var s int
		var n string
		if err := rows.Scan(&s, &n); err != nil {
			return nil, err
		}
		out[s] = n
	}
	return out, rows.Err()
}
