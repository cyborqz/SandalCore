// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package statprog

import (
	"encoding/json"
	"fmt"
	"math"
	"sort"
	"strings"

	"aocrop/services/internal/content"
)

// What an equipped item adds to the stats, from content.db: the equipment side
// of the stat engine's inputs (Inputs.Mods and Inputs.Equipped). Plan B2 will
// feed the engine from the character's items; this is the same reading, kept
// here so that the engine can be proven against the captured Bard and Ranger.
//
//   - An item's ItemStatBlock gives, per stat, a Min and a Max at each rarity
//     (Common, Uncommon, Rare, Heroic, Epic, Legendary, Artifact). The item's
//     Quality picks the rarity band of its QualityCurve (Common 1-5, Uncommon
//     6-15, Rare 16-30, Heroic 31-50, Epic 51-70, Legendary 71-100, Artifact
//     101-200) and the value is interpolated in it: Min + (Max-Min) * (q-X)/(Y-X).
//   - A SetBonus record adds, for each tier (the number of worn pieces of the set it
//     needs), stat bonuses by rarity; every tier up to the worn count applies, at
//     the lowest rarity among the worn pieces of the set.
//
// Both rules were fitted to two captured characters (the Bard exactly; the
// Ranger with effects excluded): the weaker evidence is the interpolation's
// rounding, see Rounding.

// Rounding is how an item's interpolated stat value is made a whole number.
type Rounding int

const (
	// RoundNone keeps the interpolated value (fits the level-1 Bard, whose items
	// are all at quality 1: every value is a whole number anyway).
	RoundNone Rounding = iota
	// RoundHalfUp rounds halves up: the best fit of the retail Ranger's 29 items,
	// not proven.
	RoundHalfUp
)

// Item is an equipped item: its ItemRecord, its Quality (an index into the
// record's quality curve) and the slot it is worn in (EEquipSlot's value name).
type Item struct {
	Record  int64
	Quality int
	Slot    string
}

// equipSlotNames is EEquipSlot's value names in value order: content.db's equip_slot table
// (TestSlotNamesAreTheEquipSlotTable holds the two together).
var equipSlotNames = [...]string{
	"Head", "Helmet", "Chest", "Wrists", "Hands", "Legs",
	"Feet", "Shoulders", "Back", "Belt", "RingRight", "RingLeft",
	"Necklace", "EarRight", "EarLeft", "Primary", "Secondary", "Ranged",
	"RangedOffHand", "Shirt", "Undergarment", "ToolBelt", "Face", "ResourceBag_0",
	"ResourceBag_1", "ResourceBag_2", "Pickaxe", "Pan", "Drill", "Axe",
	"Saw", "Tap", "Scythe", "Sickle", "Shears", "FishingRod",
	"Harpoon", "Net", "Bow", "Bait", "Trap", "Crowbar",
	"Chassis", "Carriage", "Wheels", "BeastOfBurden", "Harness", "Armor",
	"Utility_1", "Utility_2", "Utility_3", "Utility_4", "Utility_5", "CargoHold",
	"Ballista", "Hull", "Helm", "Radar", "Rudder", "Sail",
	"Mast", "FigureHead", "Auxiliary_1", "Auxiliary_2", "Auxiliary_3", "Auxiliary_4",
	"Auxiliary_5", "Auxiliary_6", "Auxiliary_7", "Auxiliary_8", "GroundMountCosmetic", "Costume",
	"FlyingMountCosmetic", "AquaticMountCosmetic", "FishingLure", "PrimaryCosmetic", "SecondaryCosmetic", "RangedCosmetic",
	"RangedOffHandCosmetic", "CloakCosmetic", "CaravanCosmetic", "BoatCosmetic", "None",
}

// C.O.A and Cyb

// SlotName is the EEquipSlot value name of a slot number (a storage payload's), the name
// worldd's catalog gives it ("Chest" for 2, "Primary" for 15); a number that is no
// EEquipSlot is "Slot<N>", as on the C++ side.
func SlotName(n int) string {
	if n >= 0 && n < len(equipSlotNames) {
		return equipSlotNames[n]
	}
	return fmt.Sprintf("Slot%d", n)
}

// Gear reads items from a content.db.
type Gear struct {
	db        *content.DB
	itemType  uint64
	startType uint64
	items     map[int64]*itemRec
}

type idRef struct {
	Guid   int64  `json:"Guid"`
	TypeID uint64 `json:"TypeId"`
}

type itemRec struct {
	StatBlock      idRef    `json:"StatBlockId"`
	QualityCurve   idRef    `json:"QualityCurveId"`
	SetBonuses     []idRef  `json:"SetBonusIds"`
	GameplayTags   []string `json:"GameplayTags"`
	DefaultQuality int      `json:"DefaultQuality"`
	name           string
}

// OpenGear opens content.db read-only for item lookups.
func OpenGear(path string) (*Gear, error) {
	db, err := content.Open(path)
	if err != nil {
		return nil, err
	}
	g := &Gear{db: db, items: map[int64]*itemRec{}}
	for schema, dst := range map[string]*uint64{"ItemRecordBase": &g.itemType, "StartingEquipmentRecordBase": &g.startType} {
		var tid int64
		if err := db.SQL().QueryRow(`SELECT type_id FROM record_table WHERE schema = ?`, schema).Scan(&tid); err != nil {
			db.Close()
			return nil, fmt.Errorf("statprog: no %s table in %s: %w", schema, path, err)
		}
		*dst = uint64(tid)
	}
	return g, nil
}

// Close closes the database.
func (g *Gear) Close() error { return g.db.Close() }

func (g *Gear) item(guid int64) (*itemRec, error) {
	if r, ok := g.items[guid]; ok {
		return r, nil
	}
	rec, err := g.db.Record(g.itemType, guid)
	if err != nil {
		return nil, err
	}
	r := &itemRec{name: rec.Name}
	if err := json.Unmarshal(rec.Fields, r); err != nil {
		return nil, fmt.Errorf("item %d: %w", guid, err)
	}
	g.items[guid] = r
	return r, nil
}

var rarities = []string{"Common", "Uncommon", "Rare", "Heroic", "Epic", "Legendary", "Artifact"}

type qualityBand struct{ X, Y float64 }

type qualityCurve map[string]qualityBand

func (g *Gear) qualityCurve(ref idRef) (qualityCurve, error) {
	rec, err := g.db.Record(ref.TypeID, ref.Guid)
	if err != nil {
		return nil, err
	}
	var raw map[string]json.RawMessage
	if err := json.Unmarshal(rec.Fields, &raw); err != nil {
		return nil, err
	}
	qc := qualityCurve{}
	for _, r := range rarities {
		var b qualityBand
		if err := json.Unmarshal(raw[r], &b); err != nil {
			return nil, fmt.Errorf("quality curve %d: %s: %w", ref.Guid, r, err)
		}
		qc[r] = b
	}
	return qc, nil
}

// band is the rarity whose range holds the quality, else the last rarity that
// starts at or below it.
func (qc qualityCurve) band(q float64) (rarity string, x, y float64) {
	found := false
	for _, r := range rarities {
		b := qc[r]
		if b.X <= q && q <= b.Y {
			return r, b.X, b.Y
		}
		if b.X <= q {
			rarity, x, y, found = r, b.X, b.Y, true
		}
	}
	if !found {
		b := qc["Common"]
		return "Common", b.X, b.Y
	}
	return
}

type statBlock struct {
	Stats []struct {
		Key   int64              `json:"Key"`
		Value map[string]float64 `json:"Value"`
	} `json:"Stats"`
}

type setBonus struct {
	Tiers []struct {
		Pieces int `json:"Key"`
		Value  struct {
			StatBonuses []struct {
				AffectedStat idRef `json:"AffectedStat"`
				ByRarity     []struct {
					Key   string  `json:"Key"`
					Value float64 `json:"Value"`
				} `json:"StatBonuses"`
			} `json:"StatBonuses"`
		} `json:"Value"`
	} `json:"SetStatBonuses"`
}

// C | Y | B : CoA

// Mods is the equipment's contribution to the stats: the items' stat blocks at
// their qualities plus the worn sets' bonuses, summed per stat, by stat guid.
func (g *Gear) Mods(items []Item, rounding Rounding) ([]Mod, error) {
	sum := map[int64]float64{}
	type setUse struct {
		rarities []string
	}
	sets := map[int64]*setUse{}
	setRefs := map[int64]idRef{}
	for _, it := range items {
		r, err := g.item(it.Record)
		if err != nil {
			return nil, err
		}
		var qc qualityCurve
		if r.QualityCurve.Guid != 0 {
			if qc, err = g.qualityCurve(r.QualityCurve); err != nil {
				return nil, err
			}
		}
		q := float64(it.Quality)
		if r.StatBlock.Guid != 0 {
			rec, err := g.db.Record(r.StatBlock.TypeID, r.StatBlock.Guid)
			if err != nil {
				return nil, err
			}
			var sb statBlock
			if err := json.Unmarshal(rec.Fields, &sb); err != nil {
				return nil, fmt.Errorf("stat block %d: %w", r.StatBlock.Guid, err)
			}
			for _, s := range sb.Stats {
				var val float64
				if qc != nil {
					rar, x, y := qc.band(q)
					lo, hi := s.Value[rar+"Min"], s.Value[rar+"Max"]
					t := 0.0
					if y != x {
						t = (q - x) / (y - x)
					}
					val = lo + (hi-lo)*t
				} else {
					val = s.Value["CommonMin"]
				}
				if rounding == RoundHalfUp {
					val = math.Floor(val + 0.5)
				}
				sum[s.Key] += val
			}
		}
		rar := "Common"
		if qc != nil {
			rar, _, _ = qc.band(q)
		}
		for _, s := range r.SetBonuses {
			u := sets[s.Guid]
			if u == nil {
				u = &setUse{}
				sets[s.Guid] = u
				setRefs[s.Guid] = s
			}
			u.rarities = append(u.rarities, rar)
		}
	}
	setGuids := make([]int64, 0, len(sets))
	for guid := range sets {
		setGuids = append(setGuids, guid)
	}
	sort.Slice(setGuids, func(i, j int) bool { return setGuids[i] < setGuids[j] })
	for _, guid := range setGuids {
		u := sets[guid]
		rec, err := g.db.Record(setRefs[guid].TypeID, guid)
		if err != nil {
			return nil, err
		}
		var sb setBonus
		if err := json.Unmarshal(rec.Fields, &sb); err != nil {
			return nil, fmt.Errorf("set bonus %d: %w", guid, err)
		}
		low := u.rarities[0]
		for _, r := range u.rarities {
			if rarityIndex(r) < rarityIndex(low) {
				low = r
			}
		}
		for _, tier := range sb.Tiers {
			if tier.Pieces > len(u.rarities) {
				continue
			}
			for _, b := range tier.Value.StatBonuses {
				var val float64
				for _, e := range b.ByRarity {
					if e.Key[strings.LastIndex(e.Key, ":")+1:] == low {
						val = e.Value
					}
				}
				sum[b.AffectedStat.Guid] += val
			}
		}
	}
	out := make([]Mod, 0, len(sum))
	for guid, v := range sum {
		out = append(out, Mod{Stat: guid, Value: v})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Stat < out[j].Stat })
	return out, nil
}

// built by C-O-A with Cyb

func rarityIndex(r string) int {
	for i, x := range rarities {
		if x == r {
			return i
		}
	}
	return len(rarities)
}

// Tags is the equipped-tag input of the items: every gameplay tag of each item,
// under the slot it is worn in.
func (g *Gear) Tags(items []Item) ([]EquippedTag, error) {
	var out []EquippedTag
	for _, it := range items {
		r, err := g.item(it.Record)
		if err != nil {
			return nil, err
		}
		for _, t := range r.GameplayTags {
			out = append(out, EquippedTag{Slot: it.Slot, Tag: t})
		}
	}
	return out, nil
}

// StartingKit is the items a new character of the class starts with
// (StartingEquipmentRecord: the equipped items, each at its record's default
// quality), by slot name.
func (g *Gear) StartingKit(class string) ([]Item, error) {
	rows, err := g.db.SQL().Query(`SELECT guid FROM record WHERE type_id = ? ORDER BY guid`, int64(g.startType))
	if err != nil {
		return nil, err
	}
	var guids []int64
	for rows.Next() {
		var guid int64
		if err := rows.Scan(&guid); err != nil {
			rows.Close()
			return nil, err
		}
		guids = append(guids, guid)
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}
	for _, guid := range guids {
		rec, err := g.db.Record(g.startType, guid)
		if err != nil {
			return nil, err
		}
		var f struct {
			Class         string `json:"Class"`
			EquippedItems []struct {
				Key   string `json:"Key"`
				Value struct {
					ItemID idRef `json:"ItemId"`
				} `json:"Value"`
			} `json:"EquippedItems"`
		}
		if err := json.Unmarshal(rec.Fields, &f); err != nil {
			return nil, fmt.Errorf("starting equipment %d: %w", guid, err)
		}
		if f.Class != "ECharacterClass::"+class {
			continue
		}
		var out []Item
		for _, e := range f.EquippedItems {
			r, err := g.item(e.Value.ItemID.Guid)
			if err != nil {
				return nil, err
			}
			q := r.DefaultQuality
			if q < 1 {
				q = 1
			}
			out = append(out, Item{Record: e.Value.ItemID.Guid, Quality: q, Slot: e.Key[strings.LastIndex(e.Key, ":")+1:]})
		}
		return out, nil
	}
	return nil, fmt.Errorf("statprog: no starting equipment for class %q", class)
}
