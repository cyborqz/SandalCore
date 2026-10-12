// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package charactergrpc

// Plan D1 over gRPC: SaveProgress reaches the store field by field, GetCharacter carries the experience rows with the
// items, and a server made with a starter set gives a new character the set in its bag after the kit, marked.

import (
	"context"
	"errors"
	"testing"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/character"
)

// C | O | A x C.Y.B

func TestSaveProgressPassesEveryFieldToTheStoreAndAnswersWithTheRows(t *testing.T) {
	st := &recStore{ret: fullCharacter(), xp: []character.Experience{{LevelingTypeGuid: 108651085518458, XP: 418410, Debt: 7}, {LevelingTypeGuid: 777, XP: 1, Debt: 0}}}
	c := dialRec(t, st)
	resp, err := c.SaveProgress(context.Background(), &characterv1.SaveProgressRequest{
		Id: "0123456789abcdef0123456789abcdef", Level: 10, ClassId: 7,
		Experience: []*characterv1.Experience{{LevelingTypeGuid: 108651085518458, Xp: 418410, Debt: 7}, {LevelingTypeGuid: 777, Xp: 1}},
	})
	if err != nil {
		t.Fatalf("SaveProgress = %v", err)
	}
	want := character.Progress{Level: 10, ClassID: 7, Experience: []character.Experience{
		{LevelingTypeGuid: 108651085518458, XP: 418410, Debt: 7}, {LevelingTypeGuid: 777, XP: 1, Debt: 0}}}
	if st.gotID != "0123456789abcdef0123456789abcdef" || st.gotProgress.Level != want.Level || st.gotProgress.ClassID != want.ClassID ||
		len(st.gotProgress.Experience) != 2 || st.gotProgress.Experience[0] != want.Experience[0] || st.gotProgress.Experience[1] != want.Experience[1] {
		t.Errorf("the store was given %q, %+v; want %+v", st.gotID, st.gotProgress, want)
	}
	wantProto := fullProto()
	wantProto.Experience = []*characterv1.Experience{{LevelingTypeGuid: 108651085518458, Xp: 418410, Debt: 7}, {LevelingTypeGuid: 777, Xp: 1}}
	wantProtoEqual(t, "the reply", resp.GetCharacter(), wantProto)

	// the store's refusals are the caller's codes
	for name, tc := range map[string]struct {
		err  error
		code codes.Code
	}{
		"invalid":   {character.ErrInvalid, codes.InvalidArgument},
		"not found": {character.ErrNotFound, codes.NotFound},
		"other":     {errors.New("boom"), codes.Internal},
	} {
		st := &recStore{err: tc.err}
		if _, err := dialRec(t, st).SaveProgress(context.Background(), &characterv1.SaveProgressRequest{Id: "x", Level: 2}); status.Code(err) != tc.code {
			t.Errorf("%s: %v, want %v", name, err, tc.code)
		}
	}
}

func TestGetCharacterCarriesTheExperienceWithTheItems(t *testing.T) {
	items, protoItems := someItems()
	st := &recStore{ret: fullCharacter(), items: items, xp: []character.Experience{{LevelingTypeGuid: 108651085518458, XP: 300, Debt: 0}}}
	c := dialRec(t, st)
	got, err := c.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: "0123456789abcdef0123456789abcdef"})
	if err != nil || len(got.GetCharacter().GetExperience()) != 0 {
		t.Fatalf("without include_items: %v, %v; want no experience", got, err)
	}
	got, err = c.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: "0123456789abcdef0123456789abcdef", IncludeItems: true})
	if err != nil {
		t.Fatal(err)
	}
	want := fullProto()
	want.Items = protoItems
	want.Experience = []*characterv1.Experience{{LevelingTypeGuid: 108651085518458, Xp: 300}}
	wantProtoEqual(t, "with include_items", got.GetCharacter(), want)
}

// Plan D1, stage 2: the ranks and the action bar travel with SaveProgress and come back with GetCharacter's items.
func TestSaveProgressPassesTheRanksAndTheBarAndGetCharacterCarriesThem(t *testing.T) {
	st := &recStore{ret: fullCharacter(),
		ranks: []character.TreeRanks{{TreeGuid: 6064629885900499588, RankGuids: []int64{11, 22, 33}}, {TreeGuid: 6064631673604472832, RankGuids: []int64{44}}},
		bar:   []character.ActionbarSlot{{Bank: 0, Slot: 2, UseType: 2, AbilityID: 5, SkillRankID: 11}, {Bank: 4, Slot: 3, UseType: 7, AbilityID: 9, ItemGUID: guidOf(0xaa), ItemRarity: 3, ItemQuality: 40, ComboButton: 2}}}
	c := dialRec(t, st)
	req := &characterv1.SaveProgressRequest{
		Id: "0123456789abcdef0123456789abcdef", ReplaceSkillRanks: true, ReplaceActionBar: true,
		SkillRanks: []*characterv1.TreeRanks{{TreeGuid: 6064629885900499588, RankGuids: []int64{11, 22, 33}}, {TreeGuid: 6064631673604472832, RankGuids: []int64{44}}},
		ActionBar: []*characterv1.ActionbarSlot{
			{Bank: 0, Slot: 2, UseType: 2, AbilityId: 5, SkillRankId: 11},
			{Bank: 4, Slot: 3, UseType: 7, AbilityId: 9, ItemGuid: guidOf(0xaa), ItemRarity: 3, ItemQuality: 40, ComboButton: 2},
		},
	}
	resp, err := c.SaveProgress(context.Background(), req)
	if err != nil {
		t.Fatalf("SaveProgress = %v", err)
	}
	g := st.gotProgress
	if !g.ReplaceSkillRanks || !g.ReplaceActionBar || len(g.SkillRanks) != 2 || g.SkillRanks[0].TreeGuid != 6064629885900499588 || len(g.SkillRanks[0].RankGuids) != 3 ||
		g.SkillRanks[0].RankGuids[2] != 33 || g.SkillRanks[1].RankGuids[0] != 44 || len(g.ActionBar) != 2 {
		t.Fatalf("the store was given %+v", g)
	}
	if g.ActionBar[1] != st.bar[1] || g.ActionBar[0] != st.bar[0] {
		t.Errorf("the bar reached the store as %+v, want %+v", g.ActionBar, st.bar)
	}
	if len(resp.GetCharacter().GetSkillRanks()) != 2 || len(resp.GetCharacter().GetActionBar()) != 2 ||
		!proto.Equal(resp.GetCharacter().GetActionBar()[1], req.ActionBar[1]) || !proto.Equal(resp.GetCharacter().GetSkillRanks()[0], req.SkillRanks[0]) {
		t.Errorf("the reply carries %v and %v", resp.GetCharacter().GetSkillRanks(), resp.GetCharacter().GetActionBar())
	}
	// without the flags the store is told to leave them
	st2 := &recStore{ret: fullCharacter()}
	if _, err := dialRec(t, st2).SaveProgress(context.Background(), &characterv1.SaveProgressRequest{Id: "x", Level: 3}); err != nil || st2.gotProgress.ReplaceSkillRanks || st2.gotProgress.ReplaceActionBar {
		t.Errorf("a progress without the flags: %v, %+v", err, st2.gotProgress)
	}
	// GetCharacter: only with the items
	c2 := dialRec(t, &recStore{ret: fullCharacter(), ranks: st.ranks, bar: st.bar})
	plain, err := c2.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: "0123456789abcdef0123456789abcdef"})
	if err != nil || len(plain.GetCharacter().GetSkillRanks()) != 0 || len(plain.GetCharacter().GetActionBar()) != 0 {
		t.Errorf("without include_items: %v, %v; want no ranks and no bar", plain, err)
	}
	full, err := c2.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: "0123456789abcdef0123456789abcdef", IncludeItems: true})
	if err != nil || len(full.GetCharacter().GetSkillRanks()) != 2 || len(full.GetCharacter().GetActionBar()) != 2 {
		t.Errorf("with include_items: %v, %v", full, err)
	}
}

// recSets makes a set of two bag items and remembers what it was asked for.
type recSets struct {
	asked [][2]int32
	err   error
}

func (r *recSets) StarterSet(class, level int32) ([]character.Item, error) {
	r.asked = append(r.asked, [2]int32{class, level})
	if r.err != nil {
		return nil, r.err
	}
	return []character.Item{
		{Container: character.Backpack, ItemRecordID: 6064634615524884604, InstanceGUID: guidOf(0xd1), Quantity: 1, Quality: 1, CreatedAt: itemTime},
		{Container: character.Backpack, ItemRecordID: 6064632988571664384, InstanceGUID: guidOf(0xd2), Quantity: 1, Quality: 1, CreatedAt: itemTime},
	}, nil
}

func TestCreateCharacterWithTheStarterSetPutsItInTheBagAfterTheKitAndMarksTheCharacter(t *testing.T) {
	st := &recStore{ret: fullCharacter()}
	sets := &recSets{}
	c := dialServer(t, New(st, acceptAll{}, WithKits(&recKits{}), WithStarterSet(sets)))
	resp, err := c.CreateCharacter(context.Background(), &characterv1.CreateCharacterRequest{AccountId: 41, Name: "Zed", ClassId: 7, AppearanceJson: "{}"})
	if err != nil {
		t.Fatal(err)
	}
	if len(sets.asked) != 1 || sets.asked[0] != [2]int32{7, 1} {
		t.Errorf("the set was made for %v, want class 7 at level 1", sets.asked)
	}
	// the kit (someItems: a bag item at slot 3, a worn item, a quest item) and then the set in the free bag slots 0 and 1
	got := st.gotNew.Items
	if len(got) != 5 || !st.gotNew.GMStarterSetGranted {
		t.Fatalf("the store was given %d items, marked %v; want 5 and marked", len(got), st.gotNew.GMStarterSetGranted)
	}
	if got[3].Container != character.Backpack || got[3].Slot != 0 || got[3].ItemRecordID != 6064634615524884604 || got[4].Slot != 1 {
		t.Errorf("the set's items: %+v, %+v (want bag slots 0 and 1)", got[3], got[4])
	}
	if n := len(resp.GetCharacter().GetItems()); n != 5 {
		t.Errorf("the reply carries %d items, want the kit and the set", n)
	}
	if !proto.Equal(resp.GetCharacter().GetItems()[3], &characterv1.Item{Container: characterv1.ItemContainer_ITEM_CONTAINER_BACKPACK, Slot: 0,
		ItemRecordId: 6064634615524884604, InstanceGuid: guidOf(0xd1), Quantity: 1, Quality: 1, CreatedAtMs: itemTime.UnixMilli()}) {
		t.Errorf("the reply's first set item: %v", resp.GetCharacter().GetItems()[3])
	}
	// a set that cannot be made refuses the creation; without a set maker nothing is marked
	st2 := &recStore{ret: fullCharacter()}
	c2 := dialServer(t, New(st2, acceptAll{}, WithKits(&recKits{}), WithStarterSet(&recSets{err: errors.New("no scrolls")})))
	if _, err := c2.CreateCharacter(context.Background(), &characterv1.CreateCharacterRequest{AccountId: 41, Name: "Zed", ClassId: 7}); status.Code(err) != codes.Internal || st2.gotNew.Name != "" {
		t.Errorf("a set that cannot be made: %v, stored %q", err, st2.gotNew.Name)
	}
	st3 := &recStore{ret: fullCharacter()}
	if _, err := dialWith(t, st3, &recKits{}).CreateCharacter(context.Background(), &characterv1.CreateCharacterRequest{AccountId: 41, Name: "Zed", ClassId: 7}); err != nil || st3.gotNew.GMStarterSetGranted || len(st3.gotNew.Items) != 3 {
		t.Errorf("without a set maker: %v, marked %v, %d items", err, st3.gotNew.GMStarterSetGranted, len(st3.gotNew.Items))
	}
}

// C O A - C Y B

// The game's maximum level is 25: a creation above it is refused before the starter set is chosen or anything is stored.
func TestCreateCharacterAboveTheGamesMaximumLevelIsRefused(t *testing.T) {
	st := &recStore{ret: fullCharacter()}
	sets := &recSets{}
	c := dialServer(t, New(st, acceptAll{}, WithKits(&recKits{}), WithStarterSet(sets)))
	for _, level := range []int32{character.MaxLevel + 1, 50, -1} {
		_, err := c.CreateCharacter(context.Background(), &characterv1.CreateCharacterRequest{AccountId: 41, Name: "Zed", ClassId: 7, AppearanceJson: "{}", Level: level})
		if status.Code(err) != codes.InvalidArgument {
			t.Errorf("level %d: %v, want InvalidArgument", level, err)
		}
	}
	if len(sets.asked) != 0 || st.gotNew.Name != "" {
		t.Errorf("a refused level reached the starter set %v or the store (%q)", sets.asked, st.gotNew.Name)
	}
	if _, err := c.CreateCharacter(context.Background(), &characterv1.CreateCharacterRequest{AccountId: 41, Name: "Zed", ClassId: 7, AppearanceJson: "{}", Level: character.MaxLevel}); err != nil {
		t.Errorf("level %d (the maximum) is refused: %v", character.MaxLevel, err)
	}
}
