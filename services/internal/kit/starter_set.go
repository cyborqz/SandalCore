// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package kit

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"io"
	"math"
	"sort"
	"strconv"
	"strings"
	"time"

	"aocrop/services/internal/character"
	"aocrop/services/internal/chardef"
	"aocrop/services/internal/content"
)

// Plan D1: the GM starter set (behind AOC_GM_STARTER_SET=1): the game's own GM items that take a character through the
// part-2 tests without an NPC. What it holds, by item name in content.db:
//
//	GM_Scroll_MakeMeLevel10, 15, 20, 25        using one grants the XP that reaches that level (worldd runs the item's
//	                                            action responses)
//	GM_Scroll_PrimaryArchetype<Class> (8)      changes the class
//	one GM_Scroll_Gear NN ... scroll            the gear set of the character's level band: NN is the highest band at or
//	                                            below the level (01 for levels 1-2, 03 for 3-5 ...); when a band has several
//	                                            scrolls (by armor weight and damage type) the one of the class's usual kind is
//	                                            taken (Light Magical for Bard, Cleric, Mage and Summoner, Medium Physical for
//	                                            Ranger and Rogue, Heavy Physical for Fighter and Tank), else the first by name
//
// After them come the test mounts (StarterMountNames), whichever content.db holds.
//
// Each is a fresh instance (quantity 1, the item's default quality, a new guid, the item's durability, one creation time
// for the set), as a kit item is. The caller puts them in the bag (character.PlaceInBackpack).

// StarterNames are the fixed items of the set, in the order they go into the bag.
var StarterNames = []string{
	"GM_Scroll_MakeMeLevel10", "GM_Scroll_MakeMeLevel15", "GM_Scroll_MakeMeLevel20", "GM_Scroll_MakeMeLevel25",
	"GM_Scroll_PrimaryArchetypeRanger", "GM_Scroll_PrimaryArchetypeBard", "GM_Scroll_PrimaryArchetypeCleric", "GM_Scroll_PrimaryArchetypeFighter",
	"GM_Scroll_PrimaryArchetypeRogue", "GM_Scroll_PrimaryArchetypeSummoner", "GM_Scroll_PrimaryArchetypeTank", "GM_Scroll_PrimaryArchetypeMage",
}

// StarterMountNames are the test mounts the set grants after the gear scroll (the mounts milestone, stage 3). The client's
// vendor window lists only 44 of the 101 mounts, so the set GRANTS a test selection: the two flyers a client lets anyone summon,
// all four aquatic mounts and two ground mounts.  A name content.db does not hold (a synthetic test file) is left out.
//
// The flyers are the two PTR Eagle Gryphons and no others (fix 2 of stage 3).  The client checks an item's activation
// requirement before it sends anything, and the other 44 flying mounts (activation CastAbility) are mayor rewards: their
// requirement is "Node_IsPlayerCitizenOfNode(...) && Node_IsPlayerMayor(...)" of some node, so for anyone else the client says
// nothing and never sends ServerItemUsed.  The PTR gryphons (activation SummonMount, no requirement) are summoned like a horse.
var StarterMountNames = []string{
	"Mount_Flying_PTR_ONLY", "Mount_Flying_PTR_ONLY_Tradeable",
	"Mount_Aquatic_Shoalskipper_1_ALT", "Mount_Aquatic_Tidesnapper_1_ALT", "Mount_Aquatic_Tidebreaker_1_ALT", "Mount_Aquatic_VerdantVoyager_0",
	"Mount_Grounded_TerrorBird_0", "Mount_Grounded_Skinwalker_0",
}

// gearKind is the armor weight and damage type of a class's usual gear, as the gear scrolls' names spell it.
var gearKind = map[string]string{
	"Ranger": "Medium Physical", "Bard": "Light Magical", "Cleric": "Light Magical", "Fighter": "Heavy Physical",
	"Rogue": "Medium Physical", "Summoner": "Light Magical", "Tank": "Heavy Physical", "Mage": "Light Magical",
}

// gearScroll is one GM_Scroll_Gear item: its band (the NN of the name) and the rest of its name.
type gearScroll struct {
	band int
	name string
	item entry
}

// StarterMaker makes the GM starter set. It reads content.db once, at LoadStarterSet.
type StarterMaker struct {
	fixed  []entry
	mounts []entry      // the test mounts that content.db holds, in StarterMountNames order
	gear   []gearScroll // by band, then name

	Now    func() time.Time
	Random io.Reader
}

// CoA with C_Y_B

// LoadStarterSet reads the set's items from db. It is an error for a fixed item to be missing or for there to be no gear
// scroll at all.
func LoadStarterSet(db *content.DB) (*StarterMaker, error) {
	m := &StarterMaker{}
	for _, name := range StarterNames {
		it, err := db.ItemByName(name)
		if err != nil {
			return nil, fmt.Errorf("starter set: %w", err)
		}
		m.fixed = append(m.fixed, bagEntry(it))
	}
	for _, name := range StarterMountNames {
		it, err := db.ItemByName(name)
		if err != nil {
			continue // not in this content.db (a synthetic file, or one built before contentc v11): the set goes without it
		}
		m.mounts = append(m.mounts, bagEntry(it))
	}
	scrolls, err := db.ItemsNamedLike("GM|_Scroll|_Gear %")
	if err != nil {
		return nil, fmt.Errorf("starter set: the gear scrolls: %w", err)
	}
	for _, s := range scrolls {
		rest := strings.TrimPrefix(s.Name, "GM_Scroll_Gear ")
		fields := strings.Fields(rest)
		if len(fields) == 0 {
			continue
		}
		band, err := strconv.Atoi(fields[0])
		if err != nil {
			continue // "GM_Scroll_Gearset Enchanting Kit" and the like: no band
		}
		it, err := db.Item(s.Guid)
		if err != nil {
			return nil, fmt.Errorf("starter set: %w", err)
		}
		m.gear = append(m.gear, gearScroll{band: band, name: s.Name, item: bagEntry(it)})
	}
	if len(m.gear) == 0 {
		return nil, fmt.Errorf("starter set: content.db has no GM_Scroll_Gear NN item")
	}
	sort.SliceStable(m.gear, func(i, j int) bool {
		if m.gear[i].band != m.gear[j].band {
			return m.gear[i].band < m.gear[j].band
		}
		return m.gear[i].name < m.gear[j].name
	})
	return m, nil
}

// bagEntry is a fresh instance of the item, for the bag (the slot is the caller's).
func bagEntry(it content.Item) entry {
	return entry{container: character.Backpack, slot: 0, record: it.Guid, quantity: 1, quality: int32(it.DefaultQuality),
		durability: float32(math.Floor(it.MaxDurability + 0.5))}
}

// GearScroll is the gear scroll of a class at a level: the highest band at or below the level (the lowest band when the
// level is below every band), the class's kind when the band has it, else the band's first by name.
func (m *StarterMaker) GearScroll(classID, level int32) (name string, ok bool) {
	e, name, ok := m.gearScroll(classID, level)
	_ = e
	return name, ok
}

func (m *StarterMaker) gearScroll(classID, level int32) (entry, string, bool) {
	if len(m.gear) == 0 {
		return entry{}, "", false
	}
	band := m.gear[0].band
	for _, g := range m.gear {
		if int32(g.band) <= level {
			band = g.band
		}
	}
	className, _ := chardef.ClassName(classID)
	kind := gearKind[className]
	var first *gearScroll
	for i := range m.gear {
		g := &m.gear[i]
		if g.band != band {
			continue
		}
		if first == nil {
			first = g
		}
		if kind != "" && strings.Contains(g.name, "("+kind+")") {
			return g.item, g.name, true
		}
	}
	return first.item, first.name, true
}

// StarterMounts implements character.MountSetMaker: the test mounts alone, as fresh instances in the Backpack with slot 0.
func (m *StarterMaker) StarterMounts() ([]character.Item, error) {
	return m.fresh(m.mounts)
}

// StarterSet implements character.StarterSetMaker: the set's items as fresh instances, in the Backpack with slot 0 (the
// caller places them). An error for a class that is none.
func (m *StarterMaker) StarterSet(classID, level int32) ([]character.Item, error) {
	if _, ok := chardef.ClassName(classID); !ok {
		return nil, fmt.Errorf("class %d is not a class", classID)
	}
	entries := append([]entry(nil), m.fixed...)
	if g, _, ok := m.gearScroll(classID, level); ok {
		entries = append(entries, g)
	}
	entries = append(entries, m.mounts...)
	return m.fresh(entries)
}

// fresh makes a new instance (a new guid, one creation time) of each entry.
func (m *StarterMaker) fresh(entries []entry) ([]character.Item, error) {
	now := time.Now
	if m.Now != nil {
		now = m.Now
	}
	created := now().Truncate(time.Second)
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
