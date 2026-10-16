// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

// Gathering stage 2, "Gathering data": the professions of a character (character_professions, migration 0010) on the real store (aoc_test). Every character holds
// Novice certification in all 22 CharacterProfessions (the user's rule of 2026-10-09): Create inserts the 22 rows in its transaction, a missing row reads as Novice level 1 XP 0,
// SaveProgress replaces the rows it is given (validated: tier 0-4, level 1-50, XP 0 or more, a profession of the 22, none twice), and a refusal changes nothing.

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"testing"

	"aocrop/services/internal/content"
)

const (
	mining        = 108752769996425
	lumberjacking = 108752769996426
	herbalism     = 108752770061963
)

func novices() []Profession {
	out := make([]Profession, 0, len(ProfessionGuids))
	for _, g := range ProfessionGuids {
		out = append(out, Profession{ProfessionGuid: g, CertificationTier: 0, Level: 1, XP: 0})
	}
	return out
}

func TestANewCharacterHoldsNoviceInAll22Professions(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, ClassName: "Bard", ArchetypeID: 17747, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	got, err := s.Professions(ctx, c.ID)
	if err != nil || !reflect.DeepEqual(got, novices()) {
		t.Fatalf("a new character's professions = %v, %v; want the 22 as Novice, level 1, no XP", got, err)
	}
	if len(got) != 22 {
		t.Errorf("%d professions", len(got))
	}
	// the rows are really there (not just the reader's fill-in): Create wrote them in its transaction
	var n int
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_professions WHERE character_id = $1`, c.ID).Scan(&n); err != nil || n != 22 {
		t.Errorf("%d rows in character_professions (%v), want 22", n, err)
	}
	// a character whose creation fails writes none: a name taken
	if _, err := s.Create(ctx, New{AccountID: acc, Name: "CYB", ClassID: 2, Items: smallKit()}); !errors.Is(err, ErrNameTaken) {
		t.Fatalf("the second Create = %v, want ErrNameTaken", err)
	}
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_professions`).Scan(&n); err != nil || n != 22 {
		t.Errorf("%d rows after a refused Create, want the 22 of the first character", n)
	}
	// the rows go with the character
	if err := s.Delete(ctx, acc, c.ID); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Professions(ctx, c.ID); !errors.Is(err, ErrNotFound) {
		t.Errorf("professions of a deleted character: %v", err)
	}
	if err := s.pool.QueryRow(ctx, `SELECT count(*) FROM character_professions`).Scan(&n); err != nil || n != 0 {
		t.Errorf("%d rows outlive the character", n)
	}
	if _, err := s.Professions(ctx, "nothing"); !errors.Is(err, ErrNotFound) {
		t.Errorf("professions of a malformed id: %v", err)
	}
}

// [CoA - C-Y-B]

func TestAMissingProfessionRowReadsAsNovice(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, _ := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: smallKit()})
	if _, err := s.pool.Exec(ctx, `DELETE FROM character_professions WHERE character_id = $1 AND profession_guid IN ($2, $3)`, c.ID, mining, herbalism); err != nil {
		t.Fatal(err)
	}
	if got, err := s.Professions(ctx, c.ID); err != nil || !reflect.DeepEqual(got, novices()) {
		t.Errorf("with two rows gone: %v, %v; want the 22 as Novice", got, err)
	}
	// a character made before the migration has none at all (the migration backfills; a restored backup might not)
	if _, err := s.pool.Exec(ctx, `DELETE FROM character_professions WHERE character_id = $1`, c.ID); err != nil {
		t.Fatal(err)
	}
	if got, err := s.Professions(ctx, c.ID); err != nil || !reflect.DeepEqual(got, novices()) {
		t.Errorf("with no rows: %v, %v; want the 22 as Novice", got, err)
	}
	// saving one writes that one row and the reader still shows the 22
	if _, err := s.SaveProgress(ctx, c.ID, Progress{Professions: []Profession{{mining, 1, 15, 90000}}}); err != nil {
		t.Fatal(err)
	}
	got, err := s.Professions(ctx, c.ID)
	want := novices()
	for i := range want {
		if want[i].ProfessionGuid == mining {
			want[i] = Profession{mining, 1, 15, 90000}
		}
	}
	if err != nil || !reflect.DeepEqual(got, want) {
		t.Errorf("after saving Mining = %v, %v", got, err)
	}
}

func TestSaveProgressWritesProfessionsAndReadsThemBack(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, _ := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, ClassName: "Bard", ArchetypeID: 17747, Items: smallKit()})
	other, _ := s.Create(ctx, New{AccountID: acc, Name: "other", ClassID: 4, Items: smallKit()})
	// level 25 Journeyman Mining with the T3 pickaxe's scroll, level 9 Novice Lumberjacking, in one save with the level and an XP row
	_, err := s.SaveProgress(ctx, c.ID, Progress{Level: 12, Experience: []Experience{{adventuring, 5, 0}},
		Professions: []Profession{{mining, 2, 25, 250000}, {lumberjacking, 0, 9, 32400}}})
	if err != nil {
		t.Fatal(err)
	}
	got, err := s.Professions(ctx, c.ID)
	if err != nil {
		t.Fatal(err)
	}
	want := novices()
	for i := range want {
		switch want[i].ProfessionGuid {
		case mining:
			want[i] = Profession{mining, 2, 25, 250000}
		case lumberjacking:
			want[i] = Profession{lumberjacking, 0, 9, 32400}
		}
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("professions = %v\nwant %v", got, want)
	}
	// the level and the XP rows were written with them, and no profession XP is in character_experience
	if xp, err := s.Experience(ctx, c.ID); err != nil || !reflect.DeepEqual(xp, []Experience{{adventuring, 5, 0}}) {
		t.Errorf("experience = %v, %v; want only Adventuring", xp, err)
	}
	// a second save replaces the rows it names and leaves the others
	if _, err := s.SaveProgress(ctx, c.ID, Progress{Professions: []Profession{{mining, 3, 40, 640000}, {herbalism, 1, 11, 48400}}}); err != nil {
		t.Fatal(err)
	}
	got, _ = s.Professions(ctx, c.ID)
	for i := range want {
		switch want[i].ProfessionGuid {
		case mining:
			want[i] = Profession{mining, 3, 40, 640000}
		case herbalism:
			want[i] = Profession{herbalism, 1, 11, 48400}
		}
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("professions after the second save = %v\nwant %v", got, want)
	}
	// a save with no professions leaves them alone
	if _, err := s.SaveProgress(ctx, c.ID, Progress{Level: 13}); err != nil {
		t.Fatal(err)
	}
	if again, _ := s.Professions(ctx, c.ID); !reflect.DeepEqual(again, want) {
		t.Errorf("a save without professions changed them: %v", again)
	}
	// another character's professions are its own
	if o, err := s.Professions(ctx, other.ID); err != nil || !reflect.DeepEqual(o, novices()) {
		t.Errorf("the other character's professions = %v, %v", o, err)
	}
}

// == C O A | C_Y_B ==

func TestSaveProgressRefusesBadProfessionsAndChangesNothing(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, _ := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassID: 2, Items: smallKit()})
	if _, err := s.SaveProgress(ctx, c.ID, Progress{Level: 5, Professions: []Profession{{mining, 1, 11, 100}}}); err != nil {
		t.Fatal(err)
	}
	before, _ := s.Professions(ctx, c.ID)
	for name, p := range map[string]Progress{
		"tier 5":             {Level: 6, Professions: []Profession{{mining, 5, 10, 0}}},
		"tier -1":            {Level: 6, Professions: []Profession{{mining, -1, 10, 0}}},
		"level 0":            {Level: 6, Professions: []Profession{{mining, 0, 0, 0}}},
		"level 51":           {Level: 6, Professions: []Profession{{mining, 4, 51, 0}}},
		"negative xp":        {Level: 6, Professions: []Profession{{mining, 0, 5, -1}}},
		"a profession of 0":  {Level: 6, Professions: []Profession{{0, 0, 5, 0}}},
		"an unknown guid":    {Level: 6, Professions: []Profession{{424242, 0, 5, 0}}},
		"a profession twice": {Level: 6, Professions: []Profession{{mining, 0, 5, 0}, {mining, 0, 6, 0}}},
		"one bad of two":     {Level: 6, Professions: []Profession{{lumberjacking, 0, 5, 0}, {herbalism, 9, 5, 0}}},
	} {
		if _, err := s.SaveProgress(ctx, c.ID, p); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: %v, want ErrInvalid", name, err)
		}
	}
	after, _ := s.Professions(ctx, c.ID)
	if !reflect.DeepEqual(before, after) {
		t.Errorf("a refused save changed the professions:\n%v\n%v", before, after)
	}
	if got, _ := s.Get(ctx, c.ID); got.Level != 5 {
		t.Errorf("a refused save changed the level to %d", got.Level)
	}
	// the highest a profession goes: GrandMaster, level 50
	if _, err := s.SaveProgress(ctx, c.ID, Progress{Professions: []Profession{{mining, 4, 50, 1000000}}}); err != nil {
		t.Errorf("GrandMaster level 50: %v", err)
	}
	if _, err := s.SaveProgress(ctx, "00000000000000000000000000000000", Progress{Professions: []Profession{{mining, 0, 5, 0}}}); !errors.Is(err, ErrNotFound) {
		t.Errorf("an unknown character: %v", err)
	}
}

// The 22 guids of the store are the 22 CharacterProfessions of content.db and of migration 0010 (the migration's own test holds those two together).
func TestProfessionGuidsAreContentDBs(t *testing.T) {
	path := os.Getenv("AOC_CONTENT_DB")
	if path == "" {
		path = filepath.Join("..", "..", "..", "dist", "content", "content.db")
	}
	cdb, err := content.Open(path)
	if err != nil {
		t.Skipf("no content.db: %v", err)
	}
	defer cdb.Close()
	if err := cdb.RequireVersion(); err != nil {
		t.Skip(err)
	}
	profs, err := cdb.Professions()
	if err != nil || len(profs) != 22 || len(ProfessionGuids) != 22 {
		t.Fatalf("Professions = %d, %v; the store has %d", len(profs), err, len(ProfessionGuids))
	}
	for i, p := range profs {
		if p.Guid != ProfessionGuids[i] {
			t.Errorf("profession %d: content.db has %d (%s), the store %d", i, p.Guid, p.Name, ProfessionGuids[i])
		}
	}
}

func TestValidateProfessions(t *testing.T) {
	if err := ValidateProfessions(nil); err != nil {
		t.Errorf("none: %v", err)
	}
	if err := ValidateProfessions(novices()); err != nil {
		t.Errorf("the 22 Novices: %v", err)
	}
	if err := ValidateProfessions([]Profession{{mining, 4, 50, 1 << 40}}); err != nil {
		t.Errorf("a GrandMaster: %v", err)
	}
	if err := ValidateProfessions([]Profession{{mining, 0, 1, 0}, {mining, 0, 1, 0}}); !errors.Is(err, ErrInvalid) {
		t.Errorf("a profession twice: %v", err)
	}
}
