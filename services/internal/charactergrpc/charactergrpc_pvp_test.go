// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package charactergrpc

// PvP (migration 0025) over gRPC: SaveProgress carries a character's corruption and blight to the store (with replace_pvp), GetCharacter (with the items) carries them back, and on
// the real store the whole path works: saved, read back, left alone by a save that does not ask, taken away by a save of two zeros, refused with nothing changed.

import (
	"context"
	"errors"
	"math"
	"testing"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/character"
	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"
)

// pvpStore is the recording store that also keeps a character's PvP state (the part of character.Store that PvPStore names).
type pvpStore struct {
	*recStore
	state character.PvP
	err   error
	asked int
}

var _ PvPStore = (*pvpStore)(nil)

func (s *pvpStore) PvPState(_ context.Context, _ string) (character.PvP, error) {
	s.asked++
	return s.state, s.err
}

func TestSaveProgressPassesThePvPStateToTheStore(t *testing.T) {
	st := &pvpStore{recStore: &recStore{ret: fullCharacter()}}
	c := dialRec(t, st)
	const id = "0123456789abcdef0123456789abcdef"
	if _, err := c.SaveProgress(context.Background(), &characterv1.SaveProgressRequest{Id: id, ReplacePvp: true, Pvp: &characterv1.PvpState{Corruption: 4.5, Blight: 2}}); err != nil {
		t.Fatalf("SaveProgress = %v", err)
	}
	if want := (character.PvP{Corruption: 4.5, Blight: 2}); !st.gotProgress.ReplacePvP || st.gotProgress.PvP != want {
		t.Errorf("the store was given replace=%v, %+v; want replace=true, %+v", st.gotProgress.ReplacePvP, st.gotProgress.PvP, want)
	}
	// replace with nothing: both are 0 and the flag still reaches the store (the store keeps no row for a character with neither)
	if _, err := c.SaveProgress(context.Background(), &characterv1.SaveProgressRequest{Id: id, ReplacePvp: true}); err != nil {
		t.Fatal(err)
	}
	if !st.gotProgress.ReplacePvP || st.gotProgress.PvP != (character.PvP{}) {
		t.Errorf("an empty replacement reached the store as replace=%v, %+v", st.gotProgress.ReplacePvP, st.gotProgress.PvP)
	}
	// a save that does not ask leaves it alone: no flag, and no numbers
	if _, err := c.SaveProgress(context.Background(), &characterv1.SaveProgressRequest{Id: id, Level: 4}); err != nil {
		t.Fatal(err)
	}
	if st.gotProgress.ReplacePvP || st.gotProgress.PvP != (character.PvP{}) {
		t.Errorf("a save that does not ask reached the store as replace=%v, %+v", st.gotProgress.ReplacePvP, st.gotProgress.PvP)
	}
	// numbers without the flag are refused here, before the store is asked
	if _, err := c.SaveProgress(context.Background(), &characterv1.SaveProgressRequest{Id: id, Pvp: &characterv1.PvpState{Corruption: 1}}); status.Code(err) != codes.InvalidArgument {
		t.Errorf("a PvP state without replace_pvp: %v, want InvalidArgument", err)
	}
	// a store that keeps no PvP state (an older kind) refuses a save that replaces it, and is read without it
	plain := dialRec(t, &recStore{ret: fullCharacter()})
	if _, err := plain.SaveProgress(context.Background(), &characterv1.SaveProgressRequest{Id: id, ReplacePvp: true, Pvp: &characterv1.PvpState{Corruption: 1}}); status.Code(err) != codes.FailedPrecondition {
		t.Errorf("a store without PvP state: %v, want FailedPrecondition", err)
	}
}

func TestGetCharacterCarriesThePvPStateWithTheItems(t *testing.T) {
	st := &pvpStore{recStore: &recStore{ret: fullCharacter()}, state: character.PvP{Corruption: 4.5, Blight: 2}}
	c := dialRec(t, st)
	const id = "0123456789abcdef0123456789abcdef"
	got, err := c.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: id})
	if err != nil || got.GetCharacter().GetPvp() != nil || st.asked != 0 {
		t.Fatalf("without include_items: %v, %v (the store was asked %d times); want no PvP state and no question", got, err, st.asked)
	}
	got, err = c.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: id, IncludeItems: true})
	if err != nil {
		t.Fatal(err)
	}
	if p := got.GetCharacter().GetPvp(); p.GetCorruption() != 4.5 || p.GetBlight() != 2 {
		t.Errorf("with include_items: corruption %v, blight %v; want 4.5 and 2", p.GetCorruption(), p.GetBlight())
	}
	// a character with neither has no state on the wire
	st.state = character.PvP{}
	if got, err := c.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: id, IncludeItems: true}); err != nil || got.GetCharacter().GetPvp() != nil {
		t.Errorf("a character with neither: %v, %v; want no PvP state", got, err)
	}
	// a store that keeps no PvP state is read without it, not refused
	plain := dialRec(t, &recStore{ret: fullCharacter()})
	if got, err := plain.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: id, IncludeItems: true}); err != nil || got.GetCharacter().GetPvp() != nil {
		t.Errorf("a store without PvP state: %v, %v; want the character with none", got, err)
	}
	// the store's failures are the caller's codes
	for name, tc := range map[string]struct {
		err  error
		code codes.Code
	}{
		"not found": {character.ErrNotFound, codes.NotFound},
		"other":     {errors.New("boom"), codes.Internal},
	} {
		failing := dialRec(t, &pvpStore{recStore: &recStore{ret: fullCharacter()}, err: tc.err})
		if _, err := failing.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: id, IncludeItems: true}); status.Code(err) != tc.code {
			t.Errorf("%s: %v, want %v", name, err, tc.code)
		}
	}
}

// the whole path on the real store
func TestPvPThroughTheServiceOnTheRealStore(t *testing.T) {
	pool := dbtest.Pool(t)
	ctx := context.Background()
	if err := db.Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	var account int64
	if err := pool.QueryRow(ctx, `INSERT INTO accounts (username, password_hash) VALUES ('Owner', 'x') RETURNING id`).Scan(&account); err != nil {
		t.Fatal(err)
	}
	c := dialWith(t, character.NewStore(pool), &recKits{})
	created, err := c.CreateCharacter(ctx, &characterv1.CreateCharacterRequest{AccountId: account, Name: "cyb", ClassId: 2, ClassName: "Bard", AppearanceJson: "{}"})
	if err != nil {
		t.Fatal(err)
	}
	id := created.GetCharacter().GetId()
	read := func() *characterv1.Character {
		t.Helper()
		got, err := c.GetCharacter(ctx, &characterv1.GetCharacterRequest{Id: id, IncludeItems: true})
		if err != nil {
			t.Fatalf("GetCharacter: %v", err)
		}
		return got.GetCharacter()
	}
	if got := read(); got.GetPvp() != nil {
		t.Fatalf("a new character has PvP state %v", got.GetPvp())
	}
	if _, err := c.SaveProgress(ctx, &characterv1.SaveProgressRequest{Id: id, ReplacePvp: true, Pvp: &characterv1.PvpState{Corruption: 4.25, Blight: 1.5}}); err != nil {
		t.Fatalf("SaveProgress: %v", err)
	}
	if got := read(); got.GetPvp().GetCorruption() != 4.25 || got.GetPvp().GetBlight() != 1.5 {
		t.Errorf("the character has corruption %v and blight %v; want 4.25 and 1.5", got.GetPvp().GetCorruption(), got.GetPvp().GetBlight())
	}
	// the list of the account's characters does not carry it, and neither does a read without the items
	list, err := c.ListCharacters(ctx, &characterv1.ListCharactersRequest{AccountId: account})
	if err != nil || len(list.GetCharacters()) != 1 || list.GetCharacters()[0].GetPvp() != nil {
		t.Errorf("ListCharacters = %v, %v", list, err)
	}
	if got, err := c.GetCharacter(ctx, &characterv1.GetCharacterRequest{Id: id}); err != nil || got.GetCharacter().GetPvp() != nil {
		t.Errorf("GetCharacter without the items = %v, %v", got, err)
	}
	// a save that does not ask leaves it alone
	if _, err := c.SaveProgress(ctx, &characterv1.SaveProgressRequest{Id: id, Level: 6}); err != nil {
		t.Fatal(err)
	}
	if got := read(); got.GetPvp().GetCorruption() != 4.25 || got.GetLevel() != 6 {
		t.Errorf("after a save of the level: corruption %v, level %d; want 4.25 and 6", got.GetPvp().GetCorruption(), got.GetLevel())
	}
	// refusals: InvalidArgument and nothing changed, a character that is not there is NotFound
	for name, req := range map[string]*characterv1.SaveProgressRequest{
		"a corruption that is not a number": {Id: id, Level: 9, ReplacePvp: true, Pvp: &characterv1.PvpState{Corruption: math.NaN()}},
		"a blight that is infinite":         {Id: id, Level: 9, ReplacePvp: true, Pvp: &characterv1.PvpState{Blight: math.Inf(1)}},
		"a negative corruption":             {Id: id, Level: 9, ReplacePvp: true, Pvp: &characterv1.PvpState{Corruption: -1}},
		"a blight of a million":             {Id: id, Level: 9, ReplacePvp: true, Pvp: &characterv1.PvpState{Blight: character.MaxPvPValue}},
		"numbers without the flag":          {Id: id, Level: 9, Pvp: &characterv1.PvpState{Corruption: 1}},
	} {
		if _, err := c.SaveProgress(ctx, req); status.Code(err) != codes.InvalidArgument {
			t.Errorf("%s: %v, want InvalidArgument", name, err)
		}
	}
	if got := read(); got.GetPvp().GetCorruption() != 4.25 || got.GetPvp().GetBlight() != 1.5 || got.GetLevel() != 6 {
		t.Errorf("after the refusals: corruption %v, blight %v, level %d; want 4.25, 1.5, 6", got.GetPvp().GetCorruption(), got.GetPvp().GetBlight(), got.GetLevel())
	}
	if _, err := c.SaveProgress(ctx, &characterv1.SaveProgressRequest{Id: "ffffffffffffffffffffffffffffffff", ReplacePvp: true, Pvp: &characterv1.PvpState{Corruption: 1}}); status.Code(err) != codes.NotFound {
		t.Errorf("an unknown character: %v, want NotFound", err)
	}
	// replaced whole, then taken away by a save of two zeros (a character with neither keeps no row)
	if _, err := c.SaveProgress(ctx, &characterv1.SaveProgressRequest{Id: id, ReplacePvp: true, Pvp: &characterv1.PvpState{Corruption: 0.5}}); err != nil {
		t.Fatal(err)
	}
	if got := read(); got.GetPvp().GetCorruption() != 0.5 || got.GetPvp().GetBlight() != 0 {
		t.Errorf("after the replacement: corruption %v, blight %v; want 0.5 and 0", got.GetPvp().GetCorruption(), got.GetPvp().GetBlight())
	}
	if _, err := c.SaveProgress(ctx, &characterv1.SaveProgressRequest{Id: id, ReplacePvp: true, Pvp: &characterv1.PvpState{}}); err != nil {
		t.Fatal(err)
	}
	if got := read(); got.GetPvp() != nil {
		t.Errorf("after the save of two zeros the character has PvP state %v; want none", got.GetPvp())
	}
}
