// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

// Plan D1, stage 2: the ranks a character bought and its action bar, on the real store (aoc_test): saved with SaveProgress as whole
// replacements, read back in order, kept per tree, refused when they break the rules and untouched when a save does not ask for them.

import (
	"context"
	"errors"
	"reflect"
	"strings"
	"testing"
)

const (
	bardTree      = 6064629885900499588
	universalTree = 6064631673604472832
)

func someBar() []ActionbarSlot {
	return []ActionbarSlot{
		{Bank: 0, Slot: 0, UseType: 2, AbilityID: 6064630736949673984, SkillRankID: 6064630736967303173},
		{Bank: 0, Slot: 5, UseType: 2, AbilityID: 6064630831284420608, SkillRankID: 6064630831423422476},
		{Bank: 2, Slot: 11, UseType: 3, ItemRecordID: 6064631187700000007, ItemGUID: guid(77), ItemRarity: 2, ItemQuality: 40},
		{Bank: 4, Slot: 1, UseType: 1, AbilityID: 107887054050936},
		{Bank: 4, Slot: 3, UseType: 7, AbilityID: 107258680465016, ComboButton: 3},
	}
}

// ~ C O A :: C | Y | B ~

func TestSaveProgressStoresTheRanksAndTheBarAndReadsThemBackInOrder(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	if r, err := s.SkillRanks(ctx, c.ID); err != nil || len(r) != 0 {
		t.Errorf("a new character's ranks = %v, %v; want none", r, err)
	}
	if b, err := s.ActionBar(ctx, c.ID); err != nil || len(b) != 0 {
		t.Errorf("a new character's bar = %v, %v; want none", b, err)
	}
	ranks := []TreeRanks{{TreeGuid: universalTree, RankGuids: []int64{900, 7, 55}}, {TreeGuid: bardTree, RankGuids: []int64{31, 30, 29}}}
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceSkillRanks: true, SkillRanks: ranks, ReplaceActionBar: true, ActionBar: someBar()}); err != nil {
		t.Fatal(err)
	}
	// by tree guid, each tree's ranks in the order they were bought (not sorted)
	want := []TreeRanks{{TreeGuid: bardTree, RankGuids: []int64{31, 30, 29}}, {TreeGuid: universalTree, RankGuids: []int64{900, 7, 55}}}
	if got, err := s.SkillRanks(ctx, c.ID); err != nil || !reflect.DeepEqual(got, want) {
		t.Errorf("ranks = %v, %v; want %v", got, err, want)
	}
	if got, err := s.ActionBar(ctx, c.ID); err != nil || !reflect.DeepEqual(got, someBar()) {
		t.Errorf("bar = %v, %v; want %v", got, err, someBar())
	}
	// a save that does not ask for them leaves them: the XP save of a level-up must not wipe a character's talents
	if _, err := s.SaveProgress(ctx, c.ID, Progress{Level: 5, Experience: []Experience{{adventuring, 29900, 0}}}); err != nil {
		t.Fatal(err)
	}
	if got, _ := s.SkillRanks(ctx, c.ID); !reflect.DeepEqual(got, want) {
		t.Errorf("ranks after a save without them = %v", got)
	}
	if got, _ := s.ActionBar(ctx, c.ID); !reflect.DeepEqual(got, someBar()) {
		t.Errorf("bar after a save without it = %v", got)
	}
	// a replacement is whole: a tree that is not listed has none left, a slot that is not listed is gone
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceSkillRanks: true, SkillRanks: []TreeRanks{{TreeGuid: bardTree, RankGuids: []int64{29}}},
		ReplaceActionBar: true, ActionBar: someBar()[:1]}); err != nil {
		t.Fatal(err)
	}
	if got, _ := s.SkillRanks(ctx, c.ID); !reflect.DeepEqual(got, []TreeRanks{{TreeGuid: bardTree, RankGuids: []int64{29}}}) {
		t.Errorf("ranks after a smaller replacement = %v", got)
	}
	if got, _ := s.ActionBar(ctx, c.ID); !reflect.DeepEqual(got, someBar()[:1]) {
		t.Errorf("bar after a smaller replacement = %v", got)
	}
	// an empty replacement empties them
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceSkillRanks: true, ReplaceActionBar: true}); err != nil {
		t.Fatal(err)
	}
	if r, _ := s.SkillRanks(ctx, c.ID); len(r) != 0 {
		t.Errorf("ranks after an empty replacement = %v", r)
	}
	if b, _ := s.ActionBar(ctx, c.ID); len(b) != 0 {
		t.Errorf("bar after an empty replacement = %v", b)
	}
	// the rows go with the character
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceSkillRanks: true, SkillRanks: ranks, ReplaceActionBar: true, ActionBar: someBar()}); err != nil {
		t.Fatal(err)
	}
	if err := s.Delete(ctx, acc, c.ID); err != nil {
		t.Fatal(err)
	}
	if _, err := s.SkillRanks(ctx, c.ID); !errors.Is(err, ErrNotFound) {
		t.Errorf("ranks of a deleted character: %v", err)
	}
	if _, err := s.ActionBar(ctx, c.ID); !errors.Is(err, ErrNotFound) {
		t.Errorf("bar of a deleted character: %v", err)
	}
}

func TestSaveProgressOfRanksAndBarRefusalsChangeNothing(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, _ := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: smallKit()})
	good := []TreeRanks{{TreeGuid: bardTree, RankGuids: []int64{1, 2}}}
	if _, err := s.SaveProgress(ctx, c.ID, Progress{Level: 7, ReplaceSkillRanks: true, SkillRanks: good, ReplaceActionBar: true, ActionBar: someBar()}); err != nil {
		t.Fatal(err)
	}
	many := make([]int64, MaxTreeRanks+1)
	for i := range many {
		many[i] = int64(i + 1)
	}
	var bigBar []ActionbarSlot
	for bank := int32(0); bank <= MaxBankIndex; bank++ {
		for slot := int32(0); slot <= MaxSlotIndex; slot++ {
			bigBar = append(bigBar, ActionbarSlot{Bank: bank, Slot: slot, UseType: 1, AbilityID: 5})
		}
	}
	for name, p := range map[string]Progress{
		"a tree of 0":              {ReplaceSkillRanks: true, SkillRanks: []TreeRanks{{TreeGuid: 0, RankGuids: []int64{1}}}},
		"a rank of 0":              {ReplaceSkillRanks: true, SkillRanks: []TreeRanks{{TreeGuid: bardTree, RankGuids: []int64{0}}}},
		"a negative rank":          {ReplaceSkillRanks: true, SkillRanks: []TreeRanks{{TreeGuid: bardTree, RankGuids: []int64{-4}}}},
		"a rank twice":             {ReplaceSkillRanks: true, SkillRanks: []TreeRanks{{TreeGuid: bardTree, RankGuids: []int64{5, 6, 5}}}},
		"a tree twice":             {ReplaceSkillRanks: true, SkillRanks: []TreeRanks{{TreeGuid: bardTree, RankGuids: []int64{5}}, {TreeGuid: bardTree, RankGuids: []int64{6}}}},
		"too many ranks in a tree": {ReplaceSkillRanks: true, SkillRanks: []TreeRanks{{TreeGuid: bardTree, RankGuids: many}}},
		"ranks without the flag":   {SkillRanks: good},
		"bank 20":                  {ReplaceActionBar: true, ActionBar: []ActionbarSlot{{Bank: 20, Slot: 0, UseType: 1}}},
		"bank -1":                  {ReplaceActionBar: true, ActionBar: []ActionbarSlot{{Bank: -1, Slot: 0, UseType: 1}}},
		"slot 16":                  {ReplaceActionBar: true, ActionBar: []ActionbarSlot{{Bank: 0, Slot: 16, UseType: 1}}},
		"use type 0":               {ReplaceActionBar: true, ActionBar: []ActionbarSlot{{Bank: 0, Slot: 0, UseType: 0}}},
		"use type 10":              {ReplaceActionBar: true, ActionBar: []ActionbarSlot{{Bank: 0, Slot: 0, UseType: 10}}},
		"instigator 4":             {ReplaceActionBar: true, ActionBar: []ActionbarSlot{{Bank: 0, Slot: 0, UseType: 1, Instigator: 4}}},
		"rarity 16":                {ReplaceActionBar: true, ActionBar: []ActionbarSlot{{Bank: 0, Slot: 0, UseType: 3, ItemRarity: 16}}},
		"combo 16":                 {ReplaceActionBar: true, ActionBar: []ActionbarSlot{{Bank: 0, Slot: 0, UseType: 1, ComboButton: 16}}},
		"negative quality":         {ReplaceActionBar: true, ActionBar: []ActionbarSlot{{Bank: 0, Slot: 0, UseType: 3, ItemQuality: -1}}},
		"a bad item guid":          {ReplaceActionBar: true, ActionBar: []ActionbarSlot{{Bank: 0, Slot: 0, UseType: 3, ItemGUID: "XYZ"}}},
		"an upper-case item guid":  {ReplaceActionBar: true, ActionBar: []ActionbarSlot{{Bank: 0, Slot: 0, UseType: 3, ItemGUID: strings.Repeat("AB", 16)}}},
		"a slot twice":             {ReplaceActionBar: true, ActionBar: []ActionbarSlot{{Bank: 1, Slot: 1, UseType: 1}, {Bank: 1, Slot: 1, UseType: 2}}},
		"too many slots":           {ReplaceActionBar: true, ActionBar: bigBar},
		"slots without the flag":   {ActionBar: someBar()},
	} {
		p.Level = 9 // a refusal must not even change the level of its own save
		if _, err := s.SaveProgress(ctx, c.ID, p); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: %v, want ErrInvalid", name, err)
		}
	}
	if got, _ := s.SkillRanks(ctx, c.ID); !reflect.DeepEqual(got, good) {
		t.Errorf("a refusal changed the ranks: %v", got)
	}
	if got, _ := s.ActionBar(ctx, c.ID); !reflect.DeepEqual(got, someBar()) {
		t.Errorf("a refusal changed the bar: %v", got)
	}
	if again, _ := s.Get(ctx, c.ID); again.Level != 7 {
		t.Errorf("a refusal changed the level: %d", again.Level)
	}
	if _, err := s.SaveProgress(ctx, c.ID, Progress{Level: 12, ReplaceSkillRanks: true, SkillRanks: good, ReplaceActionBar: true, ActionBar: someBar()[:2]}); err != nil {
		t.Fatal(err)
	}
	if again, _ := s.Get(ctx, c.ID); again.Level != 12 {
		t.Errorf("a good save did not change the level: %d", again.Level)
	}
	if _, err := s.SkillRanks(ctx, "ffffffffffffffffffffffffffffffff"); !errors.Is(err, ErrNotFound) {
		t.Errorf("ranks of an unknown character: %v", err)
	}
	if _, err := s.ActionBar(ctx, "not an id"); !errors.Is(err, ErrNotFound) {
		t.Errorf("bar of a malformed id: %v", err)
	}
}
