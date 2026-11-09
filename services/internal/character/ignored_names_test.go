// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

// Social, step 6 (migration 0021): the players and the guilds a character ignores. They are saved with the progress, replaced whole, read back in the order they were added, kept apart for each
// character, and removed with it. The checks of a save run on the store (aoc_test) and, for the rules about names, without a database.

import (
	"context"
	"errors"
	"fmt"
	"reflect"
	"strings"
	"testing"
)

// names makes n distinct player names, Name00, Name01 ...
func names(prefix string, n int) []string {
	out := make([]string, n)
	for i := range out {
		out[i] = fmt.Sprintf("%s%02d", prefix, i)
	}
	return out
}

func newIgnorer(t *testing.T, s *Store, acc int64, name string) Character {
	t.Helper()
	c, err := s.Create(context.Background(), New{AccountID: acc, Name: name, ClassID: 2, ClassName: "Bard", ArchetypeID: 17747, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func TestIgnoredNamesAreSavedWithTheProgressAndReadBackInOrder(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newIgnorer(t, s, acc, "cyb")
	if got, err := s.IgnoredNames(ctx, c.ID); err != nil || len(got.Characters) != 0 || len(got.Guilds) != 0 {
		t.Fatalf("a new character ignores %+v, %v; want nobody", got, err)
	}
	first := IgnoredNames{Characters: []string{"Brannoc", "Mirelle", "Tavish"}, Guilds: []string{"Sandal Makers"}}
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceIgnoredNames: true, IgnoredNames: first}); err != nil {
		t.Fatalf("SaveProgress: %v", err)
	}
	if got, err := s.IgnoredNames(ctx, c.ID); err != nil || !reflect.DeepEqual(got, first) {
		t.Errorf("the character ignores %+v, %v; want %+v in the order they were given", got, err, first)
	}
	// a second save replaces both lists whole: the order is the new list's, a name that is gone is gone
	second := IgnoredNames{Characters: []string{"Tavish", "Brannoc"}}
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceIgnoredNames: true, IgnoredNames: second}); err != nil {
		t.Fatal(err)
	}
	if got, err := s.IgnoredNames(ctx, c.ID); err != nil || !reflect.DeepEqual(got, second) {
		t.Errorf("after the second save the character ignores %+v, %v; want %+v", got, err, second)
	}
	// names are kept as they were typed, letter case and a letter outside ASCII included
	typed := IgnoredNames{Characters: []string{"BRANNOC", "brannoc2", "Zoë"}, Guilds: []string{"The  Ones"}}
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceIgnoredNames: true, IgnoredNames: typed}); err != nil {
		t.Fatal(err)
	}
	if got, err := s.IgnoredNames(ctx, c.ID); err != nil || !reflect.DeepEqual(got, typed) {
		t.Errorf("the character ignores %+v, %v; want the names as typed %+v", got, err, typed)
	}
	// an empty replacement empties both lists
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceIgnoredNames: true}); err != nil {
		t.Fatal(err)
	}
	if got, err := s.IgnoredNames(ctx, c.ID); err != nil || len(got.Characters) != 0 || len(got.Guilds) != 0 {
		t.Errorf("after an empty replacement the character ignores %+v, %v; want nobody", got, err)
	}
	var rows int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_ignored_names WHERE character_id = $1`, c.ID).Scan(&rows); err != nil || rows != 0 {
		t.Errorf("%d rows are left after the empty replacement (%v)", rows, err)
	}
}

// A save that does not ask for the names leaves them alone, whatever else it changes.
func TestASaveWithoutReplaceIgnoredNamesLeavesThemAlone(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newIgnorer(t, s, acc, "cyb")
	kept := IgnoredNames{Characters: []string{"Brannoc", "Mirelle"}, Guilds: []string{"Sandal Makers"}}
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceIgnoredNames: true, IgnoredNames: kept}); err != nil {
		t.Fatal(err)
	}
	if got, err := s.SaveProgress(ctx, c.ID, Progress{Level: 12, Experience: []Experience{{LevelingTypeGuid: adventuring, XP: 100}}}); err != nil || got.Level != 12 {
		t.Fatalf("a save of the level: %+v, %v", got, err)
	}
	if got, err := s.IgnoredNames(ctx, c.ID); err != nil || !reflect.DeepEqual(got, kept) {
		t.Errorf("after a save of the level the character ignores %+v, %v; want %+v", got, err, kept)
	}
	// and the names do not disturb the rest of the progress they travel with
	if _, err := s.SaveProgress(ctx, c.ID, Progress{Level: 13, ReplaceIgnoredNames: true, IgnoredNames: IgnoredNames{Characters: []string{"Tavish"}}}); err != nil {
		t.Fatal(err)
	}
	if got, err := s.Get(ctx, c.ID); err != nil || got.Level != 13 {
		t.Errorf("the level in the same save as the names: %+v, %v; want 13", got, err)
	}
	if xp, err := s.Experience(ctx, c.ID); err != nil || len(xp) != 1 || xp[0].XP != 100 {
		t.Errorf("the experience after a save with names: %v, %v; want the row of the first save", xp, err)
	}
}

// A refused save changes nothing: not the names, and not the rest of the progress it came with.
func TestARefusedSaveOfIgnoredNamesChangesNothing(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newIgnorer(t, s, acc, "cyb")
	kept := IgnoredNames{Characters: []string{"Brannoc"}, Guilds: []string{"Sandal Makers"}}
	if _, err := s.SaveProgress(ctx, c.ID, Progress{Level: 5, ReplaceIgnoredNames: true, IgnoredNames: kept}); err != nil {
		t.Fatal(err)
	}
	for name, p := range map[string]Progress{
		"51 players":            {Level: 9, ReplaceIgnoredNames: true, IgnoredNames: IgnoredNames{Characters: names("Name", MaxIgnoredNames+1)}},
		"51 guilds":             {Level: 9, ReplaceIgnoredNames: true, IgnoredNames: IgnoredNames{Guilds: names("Guild", MaxIgnoredNames+1)}},
		"a player twice":        {Level: 9, ReplaceIgnoredNames: true, IgnoredNames: IgnoredNames{Characters: []string{"Brannoc", "Mirelle", "Brannoc"}}},
		"a player in two cases": {Level: 9, ReplaceIgnoredNames: true, IgnoredNames: IgnoredNames{Characters: []string{"Brannoc", "BRANNOC"}}},
		"an empty name":         {Level: 9, ReplaceIgnoredNames: true, IgnoredNames: IgnoredNames{Characters: []string{""}}},
		"a name with a newline": {Level: 9, ReplaceIgnoredNames: true, IgnoredNames: IgnoredNames{Guilds: []string{"a\nb"}}},
		"names without replace": {Level: 9, IgnoredNames: IgnoredNames{Characters: []string{"Mirelle"}}},
	} {
		if _, err := s.SaveProgress(ctx, c.ID, p); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: SaveProgress = %v, want ErrInvalid", name, err)
		}
	}
	if got, err := s.IgnoredNames(ctx, c.ID); err != nil || !reflect.DeepEqual(got, kept) {
		t.Errorf("after the refusals the character ignores %+v, %v; want %+v", got, err, kept)
	}
	if got, err := s.Get(ctx, c.ID); err != nil || got.Level != 5 {
		t.Errorf("after the refusals the level is %+v, %v; want 5: a refused save writes nothing", got, err)
	}
}

func TestValidateIgnoredNames(t *testing.T) {
	longest := strings.Repeat("n", MaxIgnoredNameLength)
	for _, tc := range []struct {
		name string
		in   IgnoredNames
		bad  string // "" accepted, else a part of the reason
	}{
		{"nobody", IgnoredNames{}, ""},
		{"one player", IgnoredNames{Characters: []string{"Brannoc"}}, ""},
		{"the cap: 50 players and 50 guilds", IgnoredNames{Characters: names("Name", MaxIgnoredNames), Guilds: names("Guild", MaxIgnoredNames)}, ""},
		{"over the cap: 51 players", IgnoredNames{Characters: names("Name", MaxIgnoredNames+1)}, "51"},
		{"over the cap: 51 guilds", IgnoredNames{Guilds: names("Guild", MaxIgnoredNames+1)}, "51"},
		{"the same name as a player and as a guild", IgnoredNames{Characters: []string{"Brannoc"}, Guilds: []string{"Brannoc"}}, ""},
		{"a name with a space inside", IgnoredNames{Guilds: []string{"Sandal Makers"}}, ""},
		{"a letter outside ASCII", IgnoredNames{Characters: []string{"Zoë"}}, ""},
		{"the longest name", IgnoredNames{Characters: []string{longest}}, ""},
		{"a name over the limit", IgnoredNames{Characters: []string{longest + "n"}}, "at most"},
		{"two letters outside ASCII past the limit in bytes", IgnoredNames{Characters: []string{strings.Repeat("ë", MaxIgnoredNameLength/2+1)}}, "at most"},
		{"an empty name", IgnoredNames{Characters: []string{""}}, "empty"},
		{"a space in front", IgnoredNames{Characters: []string{" Brannoc"}}, "space"},
		{"a space behind", IgnoredNames{Guilds: []string{"Brannoc "}}, "space"},
		{"a name of spaces", IgnoredNames{Characters: []string{"   "}}, "space"},
		{"a tab inside", IgnoredNames{Characters: []string{"El\tta"}}, "control"},
		{"a newline inside", IgnoredNames{Characters: []string{"El\nta"}}, "control"},
		{"a NUL inside", IgnoredNames{Characters: []string{"El\x00ta"}}, "control"},
		{"a DEL inside", IgnoredNames{Characters: []string{"El\x7fta"}}, "control"},
		{"bytes that are no text", IgnoredNames{Characters: []string{"El\xffta"}}, "UTF-8"},
		{"a player twice", IgnoredNames{Characters: []string{"Brannoc", "Mirelle", "Brannoc"}}, "again"},
		{"a player in two cases", IgnoredNames{Characters: []string{"Brannoc", "BRANNOC"}}, "again"},
		{"a guild in two cases", IgnoredNames{Guilds: []string{"Sandal Makers", "SANDAL makers"}}, "again"},
		// only the letters of ASCII fold: the game client's list and ours agree on those, and a name outside ASCII has to match exactly
		{"letters outside ASCII in two cases are two names", IgnoredNames{Characters: []string{"élan", "Élan"}}, ""},
	} {
		err := ValidateIgnoredNames(tc.in)
		switch {
		case tc.bad == "" && err != nil:
			t.Errorf("%s: refused: %v", tc.name, err)
		case tc.bad != "" && (err == nil || !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), tc.bad)):
			t.Errorf("%s: %v, want ErrInvalid saying %q", tc.name, err, tc.bad)
		}
	}
}

// The progress checks the names only when it is asked to replace them.
func TestProgressValidatesIgnoredNamesOnlyWhenReplacing(t *testing.T) {
	if err := (Progress{Level: 3, ReplaceIgnoredNames: true, IgnoredNames: IgnoredNames{Characters: []string{"Brannoc"}}}).Validate(); err != nil {
		t.Errorf("a progress that replaces the names: %v", err)
	}
	if err := (Progress{Level: 3, ReplaceIgnoredNames: true}).Validate(); err != nil {
		t.Errorf("a progress that empties the names: %v", err)
	}
	if err := (Progress{Level: 3, IgnoredNames: IgnoredNames{Guilds: []string{"Sandal Makers"}}}).Validate(); !errors.Is(err, ErrInvalid) {
		t.Errorf("names without replace_ignored_names: %v, want ErrInvalid", err)
	}
	if err := (Progress{Level: 3, ReplaceIgnoredNames: true, IgnoredNames: IgnoredNames{Characters: []string{"Brannoc", "brannoc"}}}).Validate(); !errors.Is(err, ErrInvalid) {
		t.Errorf("a name twice in two cases: %v, want ErrInvalid", err)
	}
}

// The largest list a character can have is saved and read back whole, in order.
func TestTheLargestIgnoreListsAreSavedWhole(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newIgnorer(t, s, acc, "cyb")
	full := IgnoredNames{Characters: names("Player", MaxIgnoredNames), Guilds: names("Guild", MaxIgnoredNames)}
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceIgnoredNames: true, IgnoredNames: full}); err != nil {
		t.Fatalf("SaveProgress of %d + %d names: %v", len(full.Characters), len(full.Guilds), err)
	}
	if got, err := s.IgnoredNames(ctx, c.ID); err != nil || !reflect.DeepEqual(got, full) {
		t.Errorf("the lists came back as %d + %d names, %v; want the %d + %d that were saved, in order", len(got.Characters), len(got.Guilds), err, len(full.Characters), len(full.Guilds))
	}
}

// Each character has lists of its own.
func TestIgnoredNamesOfOneCharacterAreNotAnotherCharacters(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	a, b := newIgnorer(t, s, acc, "cyb"), newIgnorer(t, s, acc, "dee")
	listA := IgnoredNames{Characters: []string{"Brannoc", "Mirelle"}, Guilds: []string{"Sandal Makers"}}
	listB := IgnoredNames{Characters: []string{"Brannoc"}}
	for id, list := range map[string]IgnoredNames{a.ID: listA, b.ID: listB} {
		if _, err := s.SaveProgress(ctx, id, Progress{ReplaceIgnoredNames: true, IgnoredNames: list}); err != nil {
			t.Fatal(err)
		}
	}
	// the second character empties its list: the first keeps its own
	if _, err := s.SaveProgress(ctx, b.ID, Progress{ReplaceIgnoredNames: true}); err != nil {
		t.Fatal(err)
	}
	if got, err := s.IgnoredNames(ctx, a.ID); err != nil || !reflect.DeepEqual(got, listA) {
		t.Errorf("the first character ignores %+v, %v; want %+v", got, err, listA)
	}
	if got, err := s.IgnoredNames(ctx, b.ID); err != nil || len(got.Characters) != 0 {
		t.Errorf("the second character ignores %+v, %v; want nobody", got, err)
	}
}

// The names go with their character; a character that does not exist has none to read.
func TestIgnoredNamesGoWithTheirCharacterAndAnUnknownOneIsNotFound(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c := newIgnorer(t, s, acc, "cyb")
	if _, err := s.SaveProgress(ctx, c.ID, Progress{ReplaceIgnoredNames: true, IgnoredNames: IgnoredNames{Characters: []string{"Brannoc"}, Guilds: []string{"Sandal Makers"}}}); err != nil {
		t.Fatal(err)
	}
	if err := s.Delete(ctx, acc, c.ID); err != nil {
		t.Fatal(err)
	}
	var rows int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_ignored_names WHERE character_id = $1`, c.ID).Scan(&rows); err != nil || rows != 0 {
		t.Errorf("a deleted character keeps %d ignored names (%v)", rows, err)
	}
	for name, id := range map[string]string{"a deleted character": c.ID, "an unknown id": "0123456789abcdef0123456789abcdef", "a malformed id": "nope"} {
		if _, err := s.IgnoredNames(ctx, id); !errors.Is(err, ErrNotFound) {
			t.Errorf("%s: IgnoredNames = %v, want ErrNotFound", name, err)
		}
		if _, err := s.SaveProgress(ctx, id, Progress{ReplaceIgnoredNames: true, IgnoredNames: IgnoredNames{Characters: []string{"Brannoc"}}}); !errors.Is(err, ErrNotFound) {
			t.Errorf("%s: SaveProgress = %v, want ErrNotFound", name, err)
		}
	}
}
