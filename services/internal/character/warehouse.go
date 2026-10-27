// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"fmt"
	"sort"

	"github.com/jackc/pgx/v5"
)

// The warehouse (the storage milestone, step 0, migration 0017): the items a character keeps in the item chunks, the material tabs and the cargo tab of a town's warehouse, one row each in
// character_warehouse_items. worldd keeps them while the character plays and saves them with the other items, in the same transaction (SaveInventory with replace_warehouse).

// The areas of a warehouse item: the item chunks (the same in every town), the material tabs and the cargo tab of one town.
const (
	WarehouseItems     = "Items"
	WarehouseMaterials = "Materials"
	WarehouseCargo     = "Cargo"
)

// MaxWarehouseSlot is the number of slots a warehouse container can name (a tab's grid is at most 8 x 16 today).
const MaxWarehouseSlot = 1024

// MaxWarehouseItems bounds one save: every container of a character, with room to spare.
const MaxWarehouseItems = 20000

// WarehouseItem is one item instance in a character's warehouse (a row of character_warehouse_items).
type WarehouseItem struct {
	Area          string // Items, Materials or Cargo
	NodeGUID      int64  // the town's record guid for Materials and Cargo, 0 for Items
	ContainerGUID int64  // the chunk's (Items) or the tab's (Materials, Cargo) record guid
	Slot          int32  // the index in a chunk, or the row-major index of the item's top-left cell in a tab
	Item                 // the instance (its Container and Slot are not used)
}

// Validate says what is wrong with the item: the constraints of the table, checked where the message can say which.
func (w WarehouseItem) Validate() error {
	switch {
	case w.Area != WarehouseItems && w.Area != WarehouseMaterials && w.Area != WarehouseCargo:
		return fmt.Errorf("%w: warehouse area %q is not Items, Materials or Cargo", ErrInvalid, w.Area)
	case w.Area == WarehouseItems && w.NodeGUID != 0:
		return fmt.Errorf("%w: a warehouse item chunk belongs to no town (node %d)", ErrInvalid, w.NodeGUID)
	case w.Area != WarehouseItems && w.NodeGUID <= 0:
		return fmt.Errorf("%w: a %s tab belongs to a town (node %d)", ErrInvalid, w.Area, w.NodeGUID)
	case w.ContainerGUID <= 0:
		return fmt.Errorf("%w: warehouse container %d is not a record", ErrInvalid, w.ContainerGUID)
	case w.Slot < 0 || w.Slot >= MaxWarehouseSlot:
		return fmt.Errorf("%w: warehouse slot %d is not 0 to %d", ErrInvalid, w.Slot, MaxWarehouseSlot-1)
	}
	// the numbers of the instance are the bag's: check them as an item of a bag slot
	it := w.Item
	it.Container = Backpack
	it.Slot = 0
	return it.Validate()
}

// ValidateWarehouseItems checks every item, that no two share a place or an instance guid, and that none shares a guid with `bag` (the character's other items): one instance is one item.
func ValidateWarehouseItems(items []WarehouseItem, bag []Item) error {
	if len(items) > MaxWarehouseItems {
		return fmt.Errorf("%w: %d warehouse items (at most %d)", ErrInvalid, len(items), MaxWarehouseItems)
	}
	guids := make(map[string]bool, len(items)+len(bag))
	for _, b := range bag {
		guids[b.InstanceGUID] = true
	}
	places := make(map[string]bool, len(items))
	for i, w := range items {
		if err := w.Validate(); err != nil {
			return fmt.Errorf("warehouse item %d: %w", i, err)
		}
		key := fmt.Sprintf("%s/%d/%d/%d", w.Area, w.NodeGUID, w.ContainerGUID, w.Slot)
		if places[key] {
			return fmt.Errorf("%w: warehouse item %d: two items in %s container %d of node %d, slot %d", ErrInvalid, i, w.Area, w.ContainerGUID, w.NodeGUID, w.Slot)
		}
		places[key] = true
		if guids[w.InstanceGUID] {
			return fmt.Errorf("%w: warehouse item %d: instance guid %s is on another item of the character", ErrInvalid, i, w.InstanceGUID)
		}
		guids[w.InstanceGUID] = true
	}
	return nil
}

// SortWarehouseItems puts items in the order WarehouseItems returns them: by area (Items, Materials, Cargo), town, container and slot.
func SortWarehouseItems(items []WarehouseItem) {
	order := map[string]int{WarehouseItems: 0, WarehouseMaterials: 1, WarehouseCargo: 2}
	sort.SliceStable(items, func(i, j int) bool {
		a, b := items[i], items[j]
		switch {
		case order[a.Area] != order[b.Area]:
			return order[a.Area] < order[b.Area]
		case a.NodeGUID != b.NodeGUID:
			return a.NodeGUID < b.NodeGUID
		case a.ContainerGUID != b.ContainerGUID:
			return a.ContainerGUID < b.ContainerGUID
		}
		return a.Slot < b.Slot
	})
}

const warehouseColumns = `area, node_guid, container_guid, slot, item_record_id, instance_guid, quantity, quality, durability, created_at, persisted_health, persisted_mana, lock_expiration`

// WarehouseItems returns the items a character keeps in its warehouse, by area (Items, Materials, Cargo), town, container and slot. A character that stored nothing has an empty list; an
// unknown character is ErrNotFound.
func (s *Store) WarehouseItems(ctx context.Context, characterID string) ([]WarehouseItem, error) {
	if !idPattern.MatchString(characterID) {
		return nil, ErrNotFound
	}
	rows, err := s.pool.Query(ctx, `SELECT `+warehouseColumns+` FROM character_warehouse_items WHERE character_id = $1
		ORDER BY CASE area WHEN 'Items' THEN 0 WHEN 'Materials' THEN 1 ELSE 2 END, node_guid, container_guid, slot`, characterID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []WarehouseItem
	for rows.Next() {
		var w WarehouseItem
		if err := rows.Scan(&w.Area, &w.NodeGUID, &w.ContainerGUID, &w.Slot, &w.ItemRecordID, &w.InstanceGUID, &w.Quantity, &w.Quality, &w.Durability, &w.CreatedAt,
			&w.PersistedHealth, &w.PersistedMana, &w.LockExpiration); err != nil {
			return nil, err
		}
		out = append(out, w)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if len(out) == 0 {
		var exists bool
		if err := s.pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
			return nil, err
		}
		if !exists {
			return nil, ErrNotFound
		}
	}
	return out, nil
}

// replaceWarehouseItems makes the character's warehouse exactly items in tx.
func replaceWarehouseItems(ctx context.Context, tx pgx.Tx, characterID string, items []WarehouseItem) error {
	if _, err := tx.Exec(ctx, `DELETE FROM character_warehouse_items WHERE character_id = $1`, characterID); err != nil {
		return err
	}
	if len(items) == 0 {
		return nil
	}
	var batch pgx.Batch
	for _, w := range items {
		batch.Queue(`INSERT INTO character_warehouse_items (character_id, `+warehouseColumns+`) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14)`,
			characterID, w.Area, w.NodeGUID, w.ContainerGUID, w.Slot, w.ItemRecordID, w.InstanceGUID, w.Quantity, w.Quality, w.Durability, w.CreatedAt,
			w.PersistedHealth, w.PersistedMana, w.LockExpiration)
	}
	res := tx.SendBatch(ctx, &batch)
	for range items {
		if _, err := res.Exec(); err != nil {
			res.Close()
			return itemsError(err)
		}
	}
	return res.Close()
}

// SaveInventoryEverything is SaveInventoryAll plus, when replaceWarehouse, the character's warehouse made exactly warehouse, in the same transaction. The warehouse is checked against the bag's
// items (no instance on both) before anything is written.
func (s *Store) SaveInventoryEverything(ctx context.Context, characterID string, items []Item, wallet []WalletEntry, replaceWallet bool, jobs []ProcessingJob, replaceJobs bool, known []int64,
	replaceKnown bool, warehouse []WarehouseItem, replaceWarehouse bool) error {
	if !idPattern.MatchString(characterID) {
		return ErrNotFound
	}
	if replaceWallet {
		if err := ValidateWallet(wallet); err != nil {
			return err
		}
	} else {
		wallet = nil
	}
	if replaceJobs {
		if err := ValidateJobs(jobs); err != nil {
			return err
		}
	}
	if replaceKnown {
		if err := ValidateKnownRecipes(known); err != nil {
			return err
		}
	}
	if replaceWarehouse {
		if err := ValidateWarehouseItems(warehouse, items); err != nil {
			return err
		}
	} else {
		warehouse = nil
	}
	return s.saveInventory(ctx, characterID, items, wallet, jobs, replaceJobs, known, replaceKnown, warehouse, replaceWarehouse)
}
