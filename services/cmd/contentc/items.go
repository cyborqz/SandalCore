// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"fmt"
	"math"
	"sort"
	"strings"

	"aocrop/services/internal/content"
	"aocrop/services/internal/designdb"
	"aocrop/services/internal/usmap"
)

// itemRecordTypes are the record structs the item tables read (plan B2) and the TypeIds content names for them.
var itemRecordTypes = map[string]uint64{
	"ItemRecordBase":              content.TypeItem,
	"ItemStatBlockRecordBase":     content.TypeItemStatBlock,
	"QualityCurveRecordBase":      content.TypeQualityCurve,
	"StartingEquipmentRecordBase": content.TypeStartingEquipment,
	"StatTypeDefRecordBase":       content.TypeStatDef,
	"CharacterClassesRecordBase":  content.TypeCharacterClasses,
}

// rarities are the bands of an ItemStatValues, in order (the quality curve's Common ... Artifact).
var rarities = []string{"Common", "Uncommon", "Rare", "Heroic", "Epic", "Legendary", "Artifact"}

// qualityBand is a rarity's range of Quality in a QualityCurve: Quality X..Y is that rarity.
type qualityBand struct{ X, Y float64 }

// [C | O | A x C Y B]

// buildItems fills equip_slot, item, item_equip_slot and starting_equipment (plan B2) and says what it wrote in meta.
func buildItems(w *writer, src *source, m *usmap.Map, opt *Options, meta map[string]string) error {
	for schema, want := range itemRecordTypes {
		if _, err := src.need(schema); err != nil {
			return err
		}
		if got := src.typeIDs[schema]; got != want {
			return fmt.Errorf("%s has TypeId %d in the database, content expects %d", schema, got, want)
		}
	}

	slots, err := buildEquipSlots(w, m)
	if err != nil {
		return err
	}
	slotValue := map[string]int64{}
	for v, name := range slots {
		slotValue[name] = v
	}

	// The stat that is an item's durability must be the one the database calls MaxDurability.
	statNames := map[int64]string{}
	for _, r := range src.rows["StatTypeDefRecordBase"] {
		statNames[r.Guid] = r.Name
	}
	if got := statNames[content.GuidMaxDurability]; got != "MaxDurability" {
		return fmt.Errorf("stat %d is %q in the database, content names it MaxDurability", content.GuidMaxDurability, got)
	}

	bands, err := readQualityCurves(src.rows["QualityCurveRecordBase"])
	if err != nil {
		return err
	}
	blocks, err := readDurabilities(src.rows["ItemStatBlockRecordBase"])
	if err != nil {
		return err
	}

	itemSlots := map[int64][]int64{} // the slots of each item, in its record's order
	items := src.rows["ItemRecordBase"]
	known := make(map[int64]bool, len(items))
	worn, withDurability := 0, 0
	for _, r := range items {
		f := newFields(fmt.Sprintf("Item %s (%d)", r.Name, r.Guid), r.Fields)
		canEquip := f.boolean("CanEquip")
		occupiesAll := f.boolean("OccupiesAllEquipSlots")
		level := f.int("Level")
		quality := f.int("DefaultQuality")
		stack := f.int("MaxStackSize")
		statBlock, _ := f.id("StatBlockId")
		curve, _ := f.id("QualityCurveId")
		appearance, _ := f.id("AppearanceId")
		slotNames := f.list("EquipSlots")
		if f.err != nil {
			return f.err
		}
		if known[r.Guid] {
			return fmt.Errorf("two ItemRecord rows have Guid %d", r.Guid)
		}
		known[r.Guid] = true
		var values []int64
		for i, sv := range slotNames {
			name, ok := sv.(string)
			if !ok {
				return fmt.Errorf("%s: EquipSlots[%d] is %T", f.where, i, sv)
			}
			v, ok := slotValue[strings.TrimPrefix(name, "EEquipSlot::")]
			if !ok {
				return fmt.Errorf("%s: EquipSlots[%d] is %q, which is not an EEquipSlot", f.where, i, name)
			}
			values = append(values, v)
		}
		itemSlots[r.Guid] = values
		if len(values) > 0 {
			worn++
		}
		durability := 0.0
		if statBlock != 0 {
			if v, ok := blocks[statBlock]; ok {
				durability = math.Floor(v.at(bands[curve], int(quality)) + 0.5)
				withDurability++
			}
		}
		if err := w.exec(`INSERT INTO item (guid, name, level, can_equip, occupies_all_equip_slots, default_quality, max_stack_size,
			stat_block_guid, appearance_guid, max_durability, quality_curve_guid) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
			r.Guid, r.Name, level, b2i(canEquip), b2i(occupiesAll), quality, stack, statBlock, appearance, durability, curve); err != nil {
			return err
		}
		for i, v := range values {
			if err := w.exec(`INSERT INTO item_equip_slot (item_guid, ord, slot) VALUES (?, ?, ?)`, r.Guid, i, v); err != nil {
				return err
			}
		}
	}
	opt.logf("item: %d items, %d with a slot, %d with a durability", len(items), worn, withDurability)

	kits, err := buildStartingKits(w, src, slotValue, known, itemSlots)
	if err != nil {
		return err
	}
	opt.logf("starting_equipment: %d items for %d classes", kits.items, kits.classes)
	meta["typed.item"] = fmt.Sprint(len(items))
	meta["typed.starting_equipment"] = fmt.Sprint(kits.items)
	return nil
}

// == C:Y:B | C.O.A ==

// buildEquipSlots fills equip_slot from the mappings' EEquipSlot (the enumerators with a name of their own: not the
// _..._CHECK_START pseudo values, cCount or _MAX) and returns the names by value.
func buildEquipSlots(w *writer, m *usmap.Map) (map[int64]string, error) {
	e := m.Enum("EEquipSlot")
	if e == nil {
		return nil, fmt.Errorf("the mappings have no enum EEquipSlot")
	}
	names := map[int64]string{}
	for _, v := range e.Values {
		if strings.HasPrefix(v.Name, "_") || v.Name == "cCount" || strings.HasSuffix(v.Name, "_MAX") {
			continue
		}
		if _, dup := names[v.Value]; dup {
			continue // an alias of a value that has its name already (the first is kept)
		}
		names[v.Value] = v.Name
	}
	if names[2] != "Chest" || names[15] != "Primary" || names[23] != "ResourceBag_0" {
		return nil, fmt.Errorf("EEquipSlot is not the enum the client has (Chest 2, Primary 15, ResourceBag_0 23): %v", names)
	}
	values := make([]int64, 0, len(names))
	for v := range names {
		values = append(values, v)
	}
	sort.Slice(values, func(i, j int) bool { return values[i] < values[j] })
	for _, v := range values {
		if err := w.exec(`INSERT INTO equip_slot (slot, name) VALUES (?, ?)`, v, names[v]); err != nil {
			return nil, err
		}
	}
	return names, nil
}

// readQualityCurves reads every QualityCurve: a rarity's range of Quality, by the curve's Guid.
func readQualityCurves(rows []designdb.Row) (map[int64][]qualityBand, error) {
	out := map[int64][]qualityBand{}
	for _, r := range rows {
		f := newFields(fmt.Sprintf("QualityCurve %s (%d)", r.Name, r.Guid), r.Fields)
		var bands []qualityBand
		for _, rarity := range rarities {
			bf := newFields(f.where+": "+rarity, f.strukt(rarity))
			bands = append(bands, qualityBand{X: bf.float("X"), Y: bf.float("Y")})
			if err := firstErr(f, bf); err != nil {
				return nil, err
			}
		}
		out[r.Guid] = bands
	}
	return out, nil
}

// durability is an ItemStatValues' Min and Max of a stat in each rarity.
type durability struct{ min, max [7]float64 }

// at is the value at a Quality: the rarity band the quality falls in, Min + (Max - Min) * (q - X) / (Y - X). A quality
// below every band reads the first, one past them all the last; an item with no quality curve reads CommonMin.
func (d durability) at(bands []qualityBand, q int) float64 {
	if len(bands) != len(rarities) {
		return d.min[0]
	}
	band := 0
	for i, b := range bands {
		if float64(q) >= b.X {
			band = i
		}
		if float64(q) >= b.X && float64(q) <= b.Y {
			break
		}
	}
	b := bands[band]
	t := 0.0
	if b.Y != b.X {
		t = (float64(q) - b.X) / (b.Y - b.X)
	}
	t = math.Max(0, math.Min(1, t))
	return d.min[band] + (d.max[band]-d.min[band])*t
}

// readDurabilities reads the MaxDurability entry of every ItemStatBlock that has one, by the block's Guid.
func readDurabilities(rows []designdb.Row) (map[int64]durability, error) {
	out := map[int64]durability{}
	for _, r := range rows {
		f := newFields(fmt.Sprintf("ItemStatBlock %s (%d)", r.Name, r.Guid), r.Fields)
		for i, p := range f.pairs("Stats") {
			key, ok := p.Key.(int64)
			if !ok {
				return nil, fmt.Errorf("%s: Stats[%d] has a %T key", f.where, i, p.Key)
			}
			if key != content.GuidMaxDurability {
				continue
			}
			vs, ok := p.Value.(designdb.Struct)
			if !ok {
				return nil, fmt.Errorf("%s: Stats[%d] is %T", f.where, i, p.Value)
			}
			vf := newFields(fmt.Sprintf("%s: MaxDurability", f.where), vs)
			var d durability
			for j, rarity := range rarities {
				d.min[j] = vf.float(rarity + "Min")
				d.max[j] = vf.float(rarity + "Max")
			}
			if vf.err != nil {
				return nil, vf.err
			}
			out[r.Guid] = d
		}
		if f.err != nil {
			return nil, f.err
		}
	}
	return out, nil
}

type kitSummary struct{ items, classes int }

// ~ C:Y:B - C | O | A ~

// buildStartingKits fills starting_equipment from the StartingEquipment rows of the eight player classes: the equipped
// items by slot and the stored ones by position. Every item must be in the item table, every equipped one in a slot its
// record allows.
func buildStartingKits(w *writer, src *source, slotValue map[string]int64, known map[int64]bool, itemSlots map[int64][]int64) (kitSummary, error) {
	var sum kitSummary
	classGuids := map[string]int64{}
	for _, r := range src.rows["CharacterClassesRecordBase"] {
		classGuids[r.Name] = r.Guid
	}
	byClass := map[string]designdb.Row{}
	for _, r := range src.rows["StartingEquipmentRecordBase"] {
		f := newFields(fmt.Sprintf("StartingEquipment %s (%d)", r.Name, r.Guid), r.Fields)
		enum := f.str("Class")
		if f.err != nil {
			return sum, f.err
		}
		name := strings.TrimPrefix(enum, "ECharacterClass::")
		if _, dup := byClass[name]; dup {
			return sum, fmt.Errorf("two StartingEquipment rows are for class %s", name)
		}
		byClass[name] = r
	}
	for _, class := range content.PlayerClasses {
		r, ok := byClass[class]
		if !ok {
			return sum, fmt.Errorf("no StartingEquipment row for class %s", class)
		}
		guid, ok := classGuids[class]
		if !ok {
			return sum, fmt.Errorf("no CharacterClasses row named %s", class)
		}
		f := newFields(fmt.Sprintf("StartingEquipment %s (%d)", r.Name, r.Guid), r.Fields)
		equipped := f.pairs("EquippedItems")
		stored := f.list("StoredItems")
		if f.err != nil {
			return sum, f.err
		}
		variant := func(where string, v designdb.Value) (item, variation int64, err error) {
			s, ok := v.(designdb.Struct)
			if !ok {
				return 0, 0, fmt.Errorf("%s: %T is not an ItemVariant", where, v)
			}
			vf := newFields(where, s)
			item, _ = vf.id("ItemId")
			variation, _ = vf.id("VariationId")
			return item, variation, vf.err
		}
		for i, p := range equipped {
			where := fmt.Sprintf("%s: EquippedItems[%d]", f.where, i)
			name, ok := p.Key.(string)
			if !ok {
				return sum, fmt.Errorf("%s: the key is %T", where, p.Key)
			}
			slot, ok := slotValue[strings.TrimPrefix(name, "EEquipSlot::")]
			if !ok {
				return sum, fmt.Errorf("%s: %q is not an EEquipSlot", where, name)
			}
			item, variation, err := variant(where, p.Value)
			if err != nil {
				return sum, err
			}
			if !known[item] {
				return sum, fmt.Errorf("%s: item %d is not an ItemRecord", where, item)
			}
			fits := false
			for _, s := range itemSlots[item] {
				fits = fits || s == slot
			}
			if !fits {
				return sum, fmt.Errorf("%s: item %d is not worn in %s (its slots: %v)", where, item, name, itemSlots[item])
			}
			if err := w.exec(`INSERT INTO starting_equipment (class_guid, class_name, container, ord, slot, item_guid, variation_guid, quantity)
				VALUES (?, ?, ?, ?, ?, ?, ?, ?)`, guid, class, content.KitEquipment, i, slot, item, variation, 1); err != nil {
				return sum, err
			}
			sum.items++
		}
		for i, v := range stored {
			where := fmt.Sprintf("%s: StoredItems[%d]", f.where, i)
			s, ok := v.(designdb.Struct)
			if !ok {
				return sum, fmt.Errorf("%s: %T is not an ItemVariantAndQuantity", where, v)
			}
			sf := newFields(where, s)
			quantity := sf.int("Quantity")
			inner := sf.strukt("Item")
			if sf.err != nil {
				return sum, sf.err
			}
			item, variation, err := variant(where+": Item", inner)
			if err != nil {
				return sum, err
			}
			if !known[item] {
				return sum, fmt.Errorf("%s: item %d is not an ItemRecord", where, item)
			}
			if quantity < 1 {
				return sum, fmt.Errorf("%s: quantity %d", where, quantity)
			}
			if err := w.exec(`INSERT INTO starting_equipment (class_guid, class_name, container, ord, slot, item_guid, variation_guid, quantity)
				VALUES (?, ?, ?, ?, ?, ?, ?, ?)`, guid, class, content.KitBackpack, i, i, item, variation, quantity); err != nil {
				return sum, err
			}
			sum.items++
		}
		sum.classes++
	}
	return sum, nil
}
