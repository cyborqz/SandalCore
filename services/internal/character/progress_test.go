// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

// Plan D1: the progress (level, class, XP rows) and the one-time GM starter set, on the real store (aoc_test).

import (
	"context"
	"errors"
	"fmt"
	"reflect"
	"strings"
	"testing"
)

const adventuring = 108651085518458

func TestSaveProgressWritesLevelClassAndExperienceAndReadsThemBack(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, ClassName: "Bard", ArchetypeID: 17747, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	if xp, err := s.Experience(ctx, c.ID); err != nil || len(xp) != 0 {
		t.Errorf("a new character's experience = %v, %v; want none", xp, err)
	}
	// the level and an XP row
	got, err := s.SaveProgress(ctx, c.ID, Progress{Level: 10, Experience: []Experience{{LevelingTypeGuid: adventuring, XP: 418410, Debt: 0}}})
	if err != nil || got.Level != 10 || got.ClassID != 2 || got.ClassName != "Bard" {
		t.Fatalf("SaveProgress = %+v, %v", got, err)
	}
	if xp, err := s.Experience(ctx, c.ID); err != nil || !reflect.DeepEqual(xp, []Experience{{adventuring, 418410, 0}}) {
		t.Errorf("experience = %v, %v", xp, err)
	}
	// the class alone: name and archetype follow; the level and the rows stay
	got, err = s.SaveProgress(ctx, c.ID, Progress{ClassID: 7})
	if err != nil || got.Level != 10 || got.ClassID != 7 || got.ClassName != "Tank" || got.ArchetypeID != 17754 {
		t.Errorf("a class change = %+v, %v", got, err)
	}
	// a second save replaces the row of the type and adds another's
	got, err = s.SaveProgress(ctx, c.ID, Progress{Level: 15, Experience: []Experience{{adventuring, 1853010, 5}, {777, 42, 0}}})
	if err != nil || got.Level != 15 {
		t.Fatalf("SaveProgress = %+v, %v", got, err)
	}
	if xp, err := s.Experience(ctx, c.ID); err != nil || !reflect.DeepEqual(xp, []Experience{{777, 42, 0}, {adventuring, 1853010, 5}}) {
		t.Errorf("experience after the second save = %v, %v", xp, err)
	}
	// the character list and Get carry the new level and class
	if again, err := s.Get(ctx, c.ID); err != nil || again.Level != 15 || again.ClassID != 7 {
		t.Errorf("Get = %+v, %v", again, err)
	}
	// the rows go with the character
	if err := s.Delete(ctx, acc, c.ID); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Experience(ctx, c.ID); !errors.Is(err, ErrNotFound) {
		t.Errorf("experience of a deleted character: %v", err)
	}
}

// C | O | A x Cyb

func TestSaveProgressRefusalsChangeNothing(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, _ := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: smallKit()})
	if _, err := s.SaveProgress(ctx, c.ID, Progress{Level: 5, Experience: []Experience{{adventuring, 29900, 0}}}); err != nil {
		t.Fatal(err)
	}
	for name, p := range map[string]Progress{
		"level 26":      {Level: 26},
		"level 51":      {Level: 51},
		"level -1":      {Level: -1},
		"class 9":       {ClassID: 9},
		"type 0":        {Experience: []Experience{{0, 1, 0}}},
		"negative xp":   {Experience: []Experience{{adventuring, -1, 0}}},
		"negative debt": {Experience: []Experience{{adventuring, 1, -1}}},
		"a type twice":  {Experience: []Experience{{adventuring, 1, 0}, {adventuring, 2, 0}}},
	} {
		if _, err := s.SaveProgress(ctx, c.ID, p); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: %v, want ErrInvalid", name, err)
		}
	}
	if _, err := s.SaveProgress(ctx, "ffffffffffffffffffffffffffffffff", Progress{Level: 2}); !errors.Is(err, ErrNotFound) {
		t.Errorf("an unknown character: %v", err)
	}
	if _, err := s.SaveProgress(ctx, "not an id", Progress{Level: 2}); !errors.Is(err, ErrNotFound) {
		t.Errorf("a malformed id: %v", err)
	}
	if again, _ := s.Get(ctx, c.ID); again.Level != 5 || again.ClassID != 2 {
		t.Errorf("a refusal changed the character: %+v", again)
	}
	if xp, _ := s.Experience(ctx, c.ID); !reflect.DeepEqual(xp, []Experience{{adventuring, 29900, 0}}) {
		t.Errorf("a refusal changed the rows: %v", xp)
	}
}

// fakeSets makes a set of three bag items for every class and remembers what it was asked for.
type fakeSets struct {
	n     int
	asked [][2]int32
}

func (f *fakeSets) StarterSet(class, level int32) ([]Item, error) {
	if class < 1 || class > 8 {
		return nil, fmt.Errorf("class %d is not a class", class)
	}
	f.asked = append(f.asked, [2]int32{class, level})
	f.n += 10
	return []Item{item(Backpack, 0, 9001, guid(f.n+1)), item(Backpack, 0, 9002, guid(f.n+2)), item(Backpack, 0, int64(9100+level), guid(f.n+3))}, nil
}

func TestGrantMissingStarterSetsGivesTheSetOnceAndSkipsAFullBag(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	logs := captureLog(t)
	a, _ := s.Create(ctx, New{AccountID: acc, Name: "Alpha", ClassID: 2, Items: smallKit()})
	// Bravo was created with the set already
	b, _ := s.Create(ctx, New{AccountID: acc, Name: "Bravo", ClassID: 7, Items: smallKit(), GMStarterSetGranted: true})
	// Charlie's bag has one free slot
	var full []Item
	for slot := int32(0); slot < 70; slot++ {
		if slot != 7 {
			full = append(full, item(Backpack, slot, 5, guid(1000+int(slot))))
		}
	}
	c, _ := s.Create(ctx, New{AccountID: acc, Name: "Charlie", ClassID: 3, Items: full, Level: 12})
	d, _ := s.Create(ctx, New{AccountID: acc, Name: "Delta", ClassID: 0})

	sets := &fakeSets{}
	res, err := s.GrantMissingStarterSets(ctx, sets)
	if err != nil {
		t.Fatal(err)
	}
	if res.Waiting != 3 || res.Granted != 2 || res.Items != 4 || res.NoRoom != 2 || len(res.Skipped) != 1 || !strings.Contains(res.Skipped[0], "Delta") {
		t.Errorf("result = %+v", res)
	}
	// Alpha's bag held slot 3: the set takes 0, 1 and 2 (Inventory lists the bag by slot)
	got, _ := s.Inventory(ctx, a.ID)
	if len(got) != 4+3 || got[0].ItemRecordID != 9001 || got[0].Slot != 0 || got[1].Slot != 1 || got[2].Slot != 2 || got[2].ItemRecordID != 9101 || got[3].Slot != 3 {
		t.Errorf("Alpha's items after the grant: %+v", got)
	}
	if got, _ := s.Inventory(ctx, b.ID); len(got) != 4 {
		t.Errorf("Bravo, created with the set, got it again: %d items", len(got))
	}
	got, _ = s.Inventory(ctx, c.ID)
	if len(got) != 70 || got[7].ItemRecordID != 9001 || got[7].Slot != 7 {
		t.Errorf("Charlie's one free slot: %d items, slot 7 holds %d", len(got), got[7].ItemRecordID)
	}
	if got, _ := s.Inventory(ctx, d.ID); len(got) != 0 {
		t.Errorf("Delta (no class) got items: %+v", got)
	}
	if !strings.Contains(logs.String(), "GM starter set granted") || !strings.Contains(logs.String(), "no_room=2") || !strings.Contains(logs.String(), "class_id is NULL") {
		t.Errorf("the log:\n%s", logs.String())
	}
	// again: nothing to do but Delta
	again, err := s.GrantMissingStarterSets(ctx, sets)
	if err != nil || again.Waiting != 1 || again.Granted != 0 || len(again.Skipped) != 1 {
		t.Errorf("second pass = %+v, %v", again, err)
	}
	if got, _ := s.Inventory(ctx, a.ID); len(got) != 7 {
		t.Errorf("Alpha got the set twice: %d items", len(got))
	}
	// the character's level reaches the maker: Charlie (level 12) was asked for a level-12 set, whose third item did not fit
	var asked []int64
	for _, call := range sets.asked {
		asked = append(asked, int64(call[1]))
	}
	if !reflect.DeepEqual(asked, []int64{1, 12}) {
		t.Errorf("the maker was asked for levels %v, want 1 (Alpha) and 12 (Charlie), and nothing at the second pass (both are marked)", asked)
	}
}

func TestCreateWithTheStarterSetMarksTheCharacter(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: smallKit(), GMStarterSetGranted: true})
	if err != nil {
		t.Fatal(err)
	}
	var marked bool
	if err := s.pool.QueryRow(ctx, `SELECT gm_starter_set_granted_at IS NOT NULL FROM characters WHERE id = $1`, c.ID).Scan(&marked); err != nil || !marked {
		t.Errorf("created with the set but not marked (%v)", err)
	}
	d, _ := s.Create(ctx, New{AccountID: acc, Name: "other", ClassID: 2, Items: smallKit()})
	if err := s.pool.QueryRow(ctx, `SELECT gm_starter_set_granted_at IS NOT NULL FROM characters WHERE id = $1`, d.ID).Scan(&marked); err != nil || marked {
		t.Errorf("created without the set but marked (%v)", err)
	}
}

// ValidateLevel: 0 is unset, 1 to 25 is a level, nothing else is (no database needed).
func TestValidateLevel(t *testing.T) {
	for _, level := range []int32{0, 1, 24, MaxLevel} {
		if err := ValidateLevel(level); err != nil {
			t.Errorf("level %d: %v", level, err)
		}
	}
	for _, level := range []int32{-1, MaxLevel + 1, 50, 1 << 30} {
		if err := ValidateLevel(level); !errors.Is(err, ErrInvalid) {
			t.Errorf("level %d: %v, want ErrInvalid", level, err)
		}
	}
	if MaxLevel != 25 {
		t.Errorf("MaxLevel is %d: the game's maximum level is 25", MaxLevel)
	}
}

// fakeMounts is a maker that also makes the test mounts alone.
type fakeMounts struct{ fakeSets }

func (f *fakeMounts) StarterMounts() ([]Item, error) {
	f.n += 10
	return []Item{item(Backpack, 0, 9501, guid(f.n+1)), item(Backpack, 0, 9502, guid(f.n+2))}, nil
}

// (CoA + C_Y_B)

// The test mounts are kept stocked (fix 2 of stage 3: the set's flyers changed): every character with the set is given the ones it
// holds nowhere; one that holds them all is not written to; one without the set gets them with it.
func TestGrantMissingStarterMountsTopsUpACharacterWithTheSet(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	a, _ := s.Create(ctx, New{AccountID: acc, Name: "Alpha", ClassID: 2, Items: smallKit()})
	// Bravo has the set but none of the mounts: it was created before they existed
	b, _ := s.Create(ctx, New{AccountID: acc, Name: "Bravo", ClassID: 7, Items: smallKit(), GMStarterSetGranted: true})
	// Charlie has the set and one of the two mounts (an older set, or one it threw away)
	c, _ := s.Create(ctx, New{AccountID: acc, Name: "Charlie", ClassID: 3, Items: append(smallKit(), item(Backpack, 40, 9501, guid(900))), GMStarterSetGranted: true})
	m := &fakeMounts{}
	res, err := s.GrantMissingStarterMounts(ctx, m)
	if err != nil || res.Waiting != 2 || res.Granted != 2 || res.Items != 3 {
		t.Fatalf("result = %+v, %v; want Bravo's 2 and Charlie's 1", res, err)
	}
	if got, _ := s.Inventory(ctx, b.ID); len(got) != len(smallKit())+2 {
		t.Errorf("Bravo holds %d items, want the kit and the 2 mounts", len(got))
	}
	got, _ := s.Inventory(ctx, c.ID)
	records := map[int64]int{}
	for _, it := range got {
		records[it.ItemRecordID]++
	}
	if len(got) != len(smallKit())+2 || records[9501] != 1 || records[9502] != 1 {
		t.Errorf("Charlie holds %d items (%v), want the kit, its mount 9501 once and the missing 9502", len(got), records)
	}
	if got, _ := s.Inventory(ctx, a.ID); len(got) != len(smallKit()) {
		t.Errorf("Alpha, without the set, got mounts here: %d items", len(got))
	}
	// a second pass finds them all held
	if again, err := s.GrantMissingStarterMounts(ctx, m); err != nil || again.Granted != 0 || again.Items != 0 {
		t.Errorf("second pass = %+v, %v", again, err)
	}
	// a mount thrown away comes back
	if _, err := s.pool.Exec(ctx, `DELETE FROM character_items WHERE character_id = $1 AND item_record_id = 9502`, c.ID); err != nil {
		t.Fatal(err)
	}
	if again, err := s.GrantMissingStarterMounts(ctx, m); err != nil || again.Granted != 1 || again.Items != 1 {
		t.Errorf("after a mount was thrown away = %+v, %v", again, err)
	}
	// Alpha's set (with the mounts in it) marks both columns; the top-up then finds her mounts, if the maker's set holds them, or gives them
	if _, err := s.GrantMissingStarterSets(ctx, m); err != nil {
		t.Fatal(err)
	}
	if _, err := s.GrantMissingStarterMounts(ctx, m); err != nil {
		t.Fatal(err)
	}
	if again, err := s.GrantMissingStarterMounts(ctx, m); err != nil || again.Items != 0 {
		t.Errorf("after the set's backfill = %+v, %v", again, err)
	}
}
