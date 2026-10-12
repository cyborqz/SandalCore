// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package kit

import (
	"database/sql"
	"strings"
	"testing"
	"time"

	"aocrop/services/internal/character"
	"aocrop/services/internal/content"
)

// gmItems are the GM items of the synthetic content.db: the fixed names and gear scrolls of three bands.
var gmItems = []struct {
	guid int64
	name string
}{
	{2001, "GM_Scroll_MakeMeLevel10"}, {2002, "GM_Scroll_MakeMeLevel15"}, {2003, "GM_Scroll_MakeMeLevel20"}, {2004, "GM_Scroll_MakeMeLevel25"},
	{2011, "GM_Scroll_PrimaryArchetypeRanger"}, {2012, "GM_Scroll_PrimaryArchetypeBard"}, {2013, "GM_Scroll_PrimaryArchetypeCleric"}, {2014, "GM_Scroll_PrimaryArchetypeFighter"},
	{2015, "GM_Scroll_PrimaryArchetypeRogue"}, {2016, "GM_Scroll_PrimaryArchetypeSummoner"}, {2017, "GM_Scroll_PrimaryArchetypeTank"}, {2018, "GM_Scroll_PrimaryArchetypeMage"},
	{2101, "GM_Scroll_Gear 01 (Light ) Starter"},
	{2103, "GM_Scroll_Gear 03 (Heavy Physical) Bastion"}, {2104, "GM_Scroll_Gear 03 (Light Magical) Aegis"}, {2105, "GM_Scroll_Gear 03 (Medium Physical) Stalker"},
	{2109, "GM_Scroll_Gear 09 (Heavy Physical) Crown"}, {2110, "GM_Scroll_Gear 09 (Light Physical) Ash"},
	{2200, "GM_Scroll_Gearset Enchanting Kit"},
}

func withGMItems(tx *sql.Tx) {
	for _, it := range gmItems {
		if _, err := tx.Exec(`INSERT INTO item (guid, name, level, can_equip, occupies_all_equip_slots, default_quality, max_stack_size, stat_block_guid,
			appearance_guid, max_durability, quality_curve_guid) VALUES (?, ?, 1, 0, 0, 1, 1, 0, 0, 0, 0)`, it.guid, it.name); err != nil {
			panic(err)
		}
	}
}

func TestStarterSetHoldsTheScrollsAndTheGearScrollOfTheBand(t *testing.T) {
	m, err := LoadStarterSet(build(t, withGMItems))
	if err != nil {
		t.Fatal(err)
	}
	m.Now = func() time.Time { return time.Unix(1_790_000_000, 500_000_000) }
	set, err := m.StarterSet(2, 1) // a Bard at level 1
	if err != nil {
		t.Fatal(err)
	}
	var records []int64
	for _, it := range set {
		records = append(records, it.ItemRecordID)
		if it.Container != character.Backpack || it.Quantity != 1 || it.Quality != 1 || !it.CreatedAt.Equal(time.Unix(1_790_000_000, 0)) {
			t.Errorf("item %+v: want a Backpack item, quantity 1, quality 1, the whole second", it)
		}
	}
	want := []int64{2001, 2002, 2003, 2004, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2101}
	if len(records) != len(want) {
		t.Fatalf("the set holds %v, want %v", records, want)
	}
	for i := range want {
		if records[i] != want[i] {
			t.Errorf("item %d is %d, want %d", i, records[i], want[i])
		}
	}
	// the gear scroll of the band: the highest band at or below the level, the class's kind when the band has it
	for _, tc := range []struct {
		class, level int32
		want         string
	}{
		{2, 1, "GM_Scroll_Gear 01 (Light ) Starter"},      // Bard level 1: the only scroll of band 01
		{2, 2, "GM_Scroll_Gear 01 (Light ) Starter"},      // below band 03
		{2, 3, "GM_Scroll_Gear 03 (Light Magical) Aegis"}, // a Bard's kind
		{7, 5, "GM_Scroll_Gear 03 (Heavy Physical) Bastion"},
		{1, 8, "GM_Scroll_Gear 03 (Medium Physical) Stalker"},
		{2, 9, "GM_Scroll_Gear 09 (Heavy Physical) Crown"},  // band 09 has no Light Magical: the band's first by name
		{7, 25, "GM_Scroll_Gear 09 (Heavy Physical) Crown"}, // above every band: the highest
	} {
		if got, ok := m.GearScroll(tc.class, tc.level); !ok || got != tc.want {
			t.Errorf("GearScroll(class %d, level %d) = %q, %v; want %q", tc.class, tc.level, got, ok, tc.want)
		}
	}
	if _, err := m.StarterSet(9, 1); err == nil {
		t.Error("a class that is none gets a set")
	}
	// every instance has its own guid
	seen := map[string]bool{}
	for i := 0; i < 20; i++ {
		set, _ := m.StarterSet(int32(i%8+1), int32(i))
		for _, it := range set {
			if seen[it.InstanceGUID] {
				t.Fatalf("guid %s made twice", it.InstanceGUID)
			}
			seen[it.InstanceGUID] = true
		}
	}
}

// -- C-Y-B & C.O.A --

// The test mounts (the mounts milestone, stage 3) come after the gear scroll; one the file lacks is left out.
func TestStarterSetEndsWithTheTestMounts(t *testing.T) {
	withMounts := func(tx *sql.Tx) {
		withGMItems(tx)
		for i, name := range []string{StarterMountNames[0], StarterMountNames[2], StarterMountNames[len(StarterMountNames)-1]} {
			if _, err := tx.Exec(`INSERT INTO item (guid, name, level, can_equip, occupies_all_equip_slots, default_quality, max_stack_size, stat_block_guid,
				appearance_guid, max_durability, quality_curve_guid) VALUES (?, ?, 1, 0, 0, 1, 1, 0, 0, 0, 0)`, 3001+i, name); err != nil {
				panic(err)
			}
		}
	}
	m, err := LoadStarterSet(build(t, withMounts))
	if err != nil {
		t.Fatal(err)
	}
	set, err := m.StarterSet(2, 1)
	if err != nil {
		t.Fatal(err)
	}
	if len(set) != 13+3 || set[12].ItemRecordID != 2101 || set[13].ItemRecordID != 3001 || set[14].ItemRecordID != 3002 || set[15].ItemRecordID != 3003 {
		t.Errorf("the set holds %d items; want the 13 scrolls then the 3 mounts the file has: %+v", len(set), set)
	}
}

func TestLoadStarterSetRefusesAFileWithoutTheScrolls(t *testing.T) {
	if m, err := LoadStarterSet(build(t, nil)); err == nil || m != nil || !strings.Contains(err.Error(), "GM_Scroll_MakeMeLevel10") {
		t.Errorf("LoadStarterSet without the GM items = %v, %v; want an error naming the first missing scroll", m, err)
	}
	noGear := func(tx *sql.Tx) {
		withGMItems(tx)
		if _, err := tx.Exec(`DELETE FROM item WHERE name LIKE 'GM\_Scroll\_Gear %' ESCAPE '\'`); err != nil {
			panic(err)
		}
	}
	if m, err := LoadStarterSet(build(t, noGear)); err == nil || m != nil || !strings.Contains(err.Error(), "GM_Scroll_Gear") {
		t.Errorf("LoadStarterSet without a gear scroll = %v, %v; want an error", m, err)
	}
}

func TestPlaceInBackpackSkipsWhatDoesNotFit(t *testing.T) {
	m, err := LoadStarterSet(build(t, withGMItems))
	if err != nil {
		t.Fatal(err)
	}
	set, _ := m.StarterSet(2, 1)
	var existing []character.Item
	for slot := int32(0); slot < 70; slot++ {
		if slot == 3 || slot == 65 {
			continue // two free slots
		}
		existing = append(existing, character.Item{Container: character.Backpack, Slot: slot, ItemRecordID: 1, InstanceGUID: "0000000000000000000000000000000" + string(rune('1')), Quantity: 1, Quality: 1, CreatedAt: time.Now()})
	}
	placed, skipped := character.PlaceInBackpack(existing, set)
	if len(placed) != 2 || skipped != len(set)-2 || placed[0].Slot != 3 || placed[1].Slot != 65 || placed[0].ItemRecordID != 2001 || placed[1].ItemRecordID != 2002 {
		t.Errorf("placed %+v, skipped %d", placed, skipped)
	}
	// an empty bag takes them from slot 0
	placed, skipped = character.PlaceInBackpack(nil, set)
	if len(placed) != len(set) || skipped != 0 || placed[0].Slot != 0 || placed[len(placed)-1].Slot != int32(len(set)-1) {
		t.Errorf("an empty bag: placed %d from slot %d to %d, skipped %d", len(placed), placed[0].Slot, placed[len(placed)-1].Slot, skipped)
	}
}

// The real file: a level-1 Bard gets the four level scrolls, the eight class scrolls and the starter gear scroll; a level-25
// Tank gets a Heavy Physical scroll of band 23.
func TestRealStarterSet(t *testing.T) {
	m, err := LoadStarterSet(realDB(t))
	if err != nil {
		t.Fatal(err)
	}
	set, err := m.StarterSet(2, 1)
	if want := 13 + len(StarterMountNames); err != nil || len(set) != want {
		t.Fatalf("the Bard's set: %d items (want %d: the scrolls, the gear scroll and the test mounts), %v", len(set), want, err)
	}
	placed, skipped := character.PlaceInBackpack(nil, set)
	if err := character.ValidateItems(placed); err != nil || skipped != 0 {
		t.Errorf("the set placed in an empty bag is not a valid set of items: %v (%d skipped)", err, skipped)
	}
	if got, _ := m.GearScroll(2, 1); got != "GM_Scroll_Gear 01 (Light ) Starter" {
		t.Errorf("a level-1 Bard's gear scroll is %q", got)
	}
	if got, _ := m.GearScroll(7, 25); !strings.HasPrefix(got, "GM_Scroll_Gear 23 (Heavy Physical)") {
		t.Errorf("a level-25 Tank's gear scroll is %q", got)
	}
	if got, _ := m.GearScroll(8, 10); !strings.HasPrefix(got, "GM_Scroll_Gear 09 (Light Magical)") {
		t.Errorf("a level-10 Mage's gear scroll is %q", got)
	}
}

// The test flyers are the two a client lets anyone summon (fix 2 of stage 3).  The 44 flying items that activate by CastAbility carry an
// activation requirement that only a node's mayor passes ("Node_IsPlayerCitizenOfNode(...) && Node_IsPlayerMayor(...)", the archetype
// "Mount_Mayor_<node>"): the client checks it before it sends ServerItemUsed, says nothing, and the server never hears of the use.  The two
// PTR Eagle Gryphons activate by SummonMount and have no requirement.
func TestRealStarterFlyersAreTheSummonMountOnes(t *testing.T) {
	db := realDB(t)
	ms, err := db.Mounts()
	if err != nil {
		t.Fatal(err)
	}
	byName := map[string]content.Mount{}
	for _, m := range ms {
		byName[m.ItemName] = m
	}
	flyers, aquatic, ground := 0, 0, 0
	for _, name := range StarterMountNames {
		m, ok := byName[name]
		if !ok {
			t.Errorf("%s is not a mount item of content.db", name)
			continue
		}
		if m.Activation != "SummonMount" {
			t.Errorf("%s activates by %s: only SummonMount items are usable by any player", name, m.Activation)
		}
		switch m.Kind {
		case content.MountFlying:
			flyers++
		case content.MountAquatic:
			aquatic++
		case content.MountGround:
			ground++
		}
	}
	if flyers != 2 || aquatic != 4 || ground != 2 {
		t.Errorf("the set holds %d flyers, %d aquatic and %d ground mounts, want 2, 4 and 2", flyers, aquatic, ground)
	}
}
