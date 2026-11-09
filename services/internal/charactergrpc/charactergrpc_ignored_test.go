// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package charactergrpc

// Social, step 6 over gRPC: SaveProgress carries the players and the guilds a character ignores to the store, GetCharacter (with the items) carries them back, and on the real store the whole path
// works: saved, read back in order, replaced whole, left alone by a save that does not ask, refused with nothing changed.

import (
	"context"
	"errors"
	"reflect"
	"testing"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	characterv1 "aocrop/services/gen/aoc/character/v1"
	"aocrop/services/internal/character"
	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"
)

// ignoringStore is the recording store that also keeps the ignore lists (the part of character.Store that IgnoredStore names).
type ignoringStore struct {
	*recStore
	names character.IgnoredNames
	err   error
	asked int
}

var _ IgnoredStore = (*ignoringStore)(nil)

func (s *ignoringStore) IgnoredNames(_ context.Context, _ string) (character.IgnoredNames, error) {
	s.asked++
	return s.names, s.err
}

func TestSaveProgressPassesTheIgnoredNamesToTheStore(t *testing.T) {
	st := &recStore{ret: fullCharacter()}
	c := dialRec(t, st)
	const id = "0123456789abcdef0123456789abcdef"
	if _, err := c.SaveProgress(context.Background(), &characterv1.SaveProgressRequest{
		Id: id, ReplaceIgnoredNames: true,
		IgnoredCharacterNames: []string{"Brannoc", "Mirelle", "Tavish"}, IgnoredGuildNames: []string{"Sandal Makers"},
	}); err != nil {
		t.Fatalf("SaveProgress = %v", err)
	}
	want := character.IgnoredNames{Characters: []string{"Brannoc", "Mirelle", "Tavish"}, Guilds: []string{"Sandal Makers"}}
	if !st.gotProgress.ReplaceIgnoredNames || !reflect.DeepEqual(st.gotProgress.IgnoredNames, want) {
		t.Errorf("the store was given replace=%v, %+v; want replace=true, %+v", st.gotProgress.ReplaceIgnoredNames, st.gotProgress.IgnoredNames, want)
	}
	// replace with nothing empties the lists: the flag still reaches the store
	if _, err := c.SaveProgress(context.Background(), &characterv1.SaveProgressRequest{Id: id, ReplaceIgnoredNames: true}); err != nil {
		t.Fatal(err)
	}
	if !st.gotProgress.ReplaceIgnoredNames || len(st.gotProgress.IgnoredNames.Characters) != 0 || len(st.gotProgress.IgnoredNames.Guilds) != 0 {
		t.Errorf("an empty replacement reached the store as replace=%v, %+v", st.gotProgress.ReplaceIgnoredNames, st.gotProgress.IgnoredNames)
	}
	// a save that does not ask leaves the lists alone: no flag, and no names
	if _, err := c.SaveProgress(context.Background(), &characterv1.SaveProgressRequest{Id: id, Level: 4}); err != nil {
		t.Fatal(err)
	}
	if st.gotProgress.ReplaceIgnoredNames || len(st.gotProgress.IgnoredNames.Characters) != 0 || len(st.gotProgress.IgnoredNames.Guilds) != 0 {
		t.Errorf("a save that does not ask reached the store as replace=%v, %+v", st.gotProgress.ReplaceIgnoredNames, st.gotProgress.IgnoredNames)
	}
}

func TestGetCharacterCarriesTheIgnoredNamesWithTheItems(t *testing.T) {
	names := character.IgnoredNames{Characters: []string{"Brannoc", "Mirelle"}, Guilds: []string{"Sandal Makers"}}
	st := &ignoringStore{recStore: &recStore{ret: fullCharacter()}, names: names}
	c := dialRec(t, st)
	const id = "0123456789abcdef0123456789abcdef"
	got, err := c.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: id})
	if err != nil || len(got.GetCharacter().GetIgnoredCharacterNames()) != 0 || len(got.GetCharacter().GetIgnoredGuildNames()) != 0 || st.asked != 0 {
		t.Fatalf("without include_items: %v, %v (the store was asked %d times); want no names and no question", got, err, st.asked)
	}
	got, err = c.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: id, IncludeItems: true})
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(got.GetCharacter().GetIgnoredCharacterNames(), names.Characters) || !reflect.DeepEqual(got.GetCharacter().GetIgnoredGuildNames(), names.Guilds) {
		t.Errorf("with include_items: players %v, guilds %v; want %v and %v", got.GetCharacter().GetIgnoredCharacterNames(), got.GetCharacter().GetIgnoredGuildNames(), names.Characters, names.Guilds)
	}
	// a store that keeps no lists (an older kind) is read without them, not refused
	plain := dialRec(t, &recStore{ret: fullCharacter()})
	if got, err := plain.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: id, IncludeItems: true}); err != nil || len(got.GetCharacter().GetIgnoredCharacterNames()) != 0 {
		t.Errorf("a store without lists: %v, %v; want the character with none", got, err)
	}
	// the store's failures are the caller's codes
	for name, tc := range map[string]struct {
		err  error
		code codes.Code
	}{
		"not found": {character.ErrNotFound, codes.NotFound},
		"other":     {errors.New("boom"), codes.Internal},
	} {
		failing := dialRec(t, &ignoringStore{recStore: &recStore{ret: fullCharacter()}, err: tc.err})
		if _, err := failing.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: id, IncludeItems: true}); status.Code(err) != tc.code {
			t.Errorf("%s: %v, want %v", name, err, tc.code)
		}
	}
}

// the whole path on the real store
func TestIgnoredNamesThroughTheServiceOnTheRealStore(t *testing.T) {
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
	if got := read(); len(got.GetIgnoredCharacterNames()) != 0 || len(got.GetIgnoredGuildNames()) != 0 {
		t.Fatalf("a new character ignores %v and %v", got.GetIgnoredCharacterNames(), got.GetIgnoredGuildNames())
	}
	players, guilds := []string{"Brannoc", "Mirelle", "Tavish"}, []string{"Sandal Makers"}
	if _, err := c.SaveProgress(ctx, &characterv1.SaveProgressRequest{Id: id, ReplaceIgnoredNames: true, IgnoredCharacterNames: players, IgnoredGuildNames: guilds}); err != nil {
		t.Fatalf("SaveProgress: %v", err)
	}
	if got := read(); !reflect.DeepEqual(got.GetIgnoredCharacterNames(), players) || !reflect.DeepEqual(got.GetIgnoredGuildNames(), guilds) {
		t.Errorf("the character ignores %v and %v; want %v and %v in that order", got.GetIgnoredCharacterNames(), got.GetIgnoredGuildNames(), players, guilds)
	}
	// the list of the account's characters does not carry them, and neither does a read without the items
	list, err := c.ListCharacters(ctx, &characterv1.ListCharactersRequest{AccountId: account})
	if err != nil || len(list.GetCharacters()) != 1 || len(list.GetCharacters()[0].GetIgnoredCharacterNames()) != 0 {
		t.Errorf("ListCharacters = %v, %v", list, err)
	}
	if got, err := c.GetCharacter(ctx, &characterv1.GetCharacterRequest{Id: id}); err != nil || len(got.GetCharacter().GetIgnoredCharacterNames()) != 0 {
		t.Errorf("GetCharacter without the items = %v, %v", got, err)
	}
	// a save that does not ask leaves them alone
	if _, err := c.SaveProgress(ctx, &characterv1.SaveProgressRequest{Id: id, Level: 6}); err != nil {
		t.Fatal(err)
	}
	if got := read(); !reflect.DeepEqual(got.GetIgnoredCharacterNames(), players) {
		t.Errorf("after a save of the level the character ignores %v; want %v", got.GetIgnoredCharacterNames(), players)
	}
	// refusals: InvalidArgument and nothing changed, a character that is not there is NotFound
	var tooMany []string
	for i := 0; i < character.MaxIgnoredNames+1; i++ {
		tooMany = append(tooMany, "Name"+string(rune('a'+i/26))+string(rune('a'+i%26)))
	}
	for name, req := range map[string]*characterv1.SaveProgressRequest{
		"51 players":             {Id: id, Level: 9, ReplaceIgnoredNames: true, IgnoredCharacterNames: tooMany},
		"a player in two cases":  {Id: id, Level: 9, ReplaceIgnoredNames: true, IgnoredCharacterNames: []string{"Brannoc", "BRANNOC"}},
		"an empty name":          {Id: id, Level: 9, ReplaceIgnoredNames: true, IgnoredGuildNames: []string{""}},
		"names without the flag": {Id: id, Level: 9, IgnoredCharacterNames: []string{"Brannoc"}},
		"a name with a new line": {Id: id, Level: 9, ReplaceIgnoredNames: true, IgnoredCharacterNames: []string{"a\nb"}},
	} {
		if _, err := c.SaveProgress(ctx, req); status.Code(err) != codes.InvalidArgument {
			t.Errorf("%s: %v, want InvalidArgument", name, err)
		}
	}
	if got := read(); !reflect.DeepEqual(got.GetIgnoredCharacterNames(), players) || !reflect.DeepEqual(got.GetIgnoredGuildNames(), guilds) || got.GetLevel() != 6 {
		t.Errorf("after the refusals: players %v, guilds %v, level %d; want %v, %v, 6", got.GetIgnoredCharacterNames(), got.GetIgnoredGuildNames(), got.GetLevel(), players, guilds)
	}
	if _, err := c.SaveProgress(ctx, &characterv1.SaveProgressRequest{Id: "ffffffffffffffffffffffffffffffff", ReplaceIgnoredNames: true, IgnoredCharacterNames: []string{"Brannoc"}}); status.Code(err) != codes.NotFound {
		t.Errorf("an unknown character: %v, want NotFound", err)
	}
	// replaced whole, then emptied
	if _, err := c.SaveProgress(ctx, &characterv1.SaveProgressRequest{Id: id, ReplaceIgnoredNames: true, IgnoredCharacterNames: []string{"Tavish"}}); err != nil {
		t.Fatal(err)
	}
	if got := read(); !reflect.DeepEqual(got.GetIgnoredCharacterNames(), []string{"Tavish"}) || len(got.GetIgnoredGuildNames()) != 0 {
		t.Errorf("after the replacement the character ignores %v and %v; want [Tavish] and none", got.GetIgnoredCharacterNames(), got.GetIgnoredGuildNames())
	}
	if _, err := c.SaveProgress(ctx, &characterv1.SaveProgressRequest{Id: id, ReplaceIgnoredNames: true}); err != nil {
		t.Fatal(err)
	}
	if got := read(); len(got.GetIgnoredCharacterNames()) != 0 {
		t.Errorf("after the empty replacement the character ignores %v", got.GetIgnoredCharacterNames())
	}
}
