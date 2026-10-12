// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package kit makes the starting kit of a new character (plan B2): the items its class starts with, from content.db's
// StartingEquipmentRecord, each a fresh instance.
//
// What a fresh instance is (the captured original server's Bard, whose 13 items are exactly its class's record):
// quantity 1, the item's default quality (1), a new random instance guid, a durability that is the item's MaxDurability stat
// at that quality rounded half up (11 for a starter armor piece, 12 for the Book, 38 for an artisan tool, 0 for an item
// with none: the Bard's helmet and the bag), and one creation time for the whole kit, a whole second.
//
// An item of EquippedItems goes to the Equipment storage, in the slot the record names (the same item may be named for two
// slots, as a Mage's Focus is: two instances); an item of StoredItems goes to the Backpack from slot 0, in the record's order.
package kit

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"io"
	"math"
	"sync"
	"time"

	"aocrop/services/internal/character"
	"aocrop/services/internal/chardef"
	"aocrop/services/internal/content"
)

// entry is one item of a class's kit: what a fresh instance of it is, but for its guid and creation time.
type entry struct {
	container  string
	slot       int32
	record     int64
	quantity   int32
	quality    int32
	durability float32
}

// Maker makes the kit of each class. It reads content.db once, at Load.
type Maker struct {
	kits map[int32][]entry

	// Now and Random stand in for the clock and the source of guids (tests).
	Now    func() time.Time
	Random io.Reader
	mu     sync.Mutex
}

// (C.Y.B ~ C | O | A)

// Load reads the eight classes' kits and the items in them from db. It is an error for a class to have no kit, for a kit
// item to be unknown, or for the kit to put an item where the record says it cannot go.
func Load(db *content.DB) (*Maker, error) {
	m := &Maker{kits: map[int32][]entry{}}
	for class := int32(1); ; class++ {
		name, ok := chardef.ClassName(class)
		if !ok {
			break
		}
		rows, err := db.StartingKit(name)
		if err != nil {
			return nil, fmt.Errorf("kit: %w", err)
		}
		var entries []entry
		for _, k := range rows {
			it, err := db.Item(k.ItemGuid)
			if err != nil {
				return nil, fmt.Errorf("kit: the %s kit: %w", name, err)
			}
			e := entry{container: character.Backpack, slot: int32(k.Slot), record: k.ItemGuid, quantity: int32(k.Quantity),
				quality: int32(it.DefaultQuality), durability: float32(math.Floor(it.MaxDurability + 0.5))}
			if k.Container == content.KitEquipment {
				e.container = character.Equipment
				fits := false
				for _, s := range it.EquipSlots {
					fits = fits || s == k.Slot
				}
				if !fits {
					return nil, fmt.Errorf("kit: the %s kit puts %s in slot %d, which its record does not allow (%v)", name, it.Name, k.Slot, it.EquipSlots)
				}
			}
			entries = append(entries, e)
		}
		probe := make([]character.Item, 0, len(entries))
		for _, e := range entries {
			probe = append(probe, e.item("0123456789abcdef0123456789abcdef", time.Unix(1_700_000_000, 0)))
		}
		for i := range probe {
			probe[i].InstanceGUID = fmt.Sprintf("%032x", i+1)
		}
		if err := character.ValidateItems(probe); err != nil {
			return nil, fmt.Errorf("kit: the %s kit: %w", name, err)
		}
		m.kits[class] = entries
	}
	if len(m.kits) != 8 {
		return nil, fmt.Errorf("kit: %d classes have a kit, want 8", len(m.kits))
	}
	return m, nil
}

func (e entry) item(guid string, created time.Time) character.Item {
	return character.Item{Container: e.container, Slot: e.slot, ItemRecordID: e.record, InstanceGUID: guid, Quantity: e.quantity,
		Quality: e.quality, Durability: e.durability, CreatedAt: created}
}

// Kit is the starting kit of a class (ECharacterClass 1-8) as fresh instances: the equipped items in the record's order, then
// the stored ones. An error for a class that has none.
func (m *Maker) Kit(classID int32) ([]character.Item, error) {
	entries, ok := m.kits[classID]
	if !ok {
		return nil, fmt.Errorf("class %d has no starting kit", classID)
	}
	now := time.Now
	if m.Now != nil {
		now = m.Now
	}
	created := now().Truncate(time.Second)
	m.mu.Lock()
	defer m.mu.Unlock()
	src := m.Random
	if src == nil {
		src = rand.Reader
	}
	out := make([]character.Item, 0, len(entries))
	for _, e := range entries {
		var raw [16]byte
		for {
			if _, err := io.ReadFull(src, raw[:]); err != nil {
				return nil, err
			}
			if raw != [16]byte{} {
				break
			}
		}
		out = append(out, e.item(hex.EncodeToString(raw[:]), created))
	}
	return out, nil
}
