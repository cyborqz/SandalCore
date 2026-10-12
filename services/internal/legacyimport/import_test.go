// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package legacyimport

import (
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"reflect"
	"strings"
	"testing"

	"aocrop/services/internal/account"
	"aocrop/services/internal/character"
	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"
)

func hexID(c string) string { return strings.Repeat(c, 32) }

// importSetup returns the real store on a fresh schema (skipped without
// AOC_TEST_DATABASE_URL, like the other store tests) and two accounts.
func importSetup(t *testing.T) (store *character.Store, me, other int64) {
	t.Helper()
	pool := dbtest.Pool(t)
	ctx := context.Background()
	if err := db.Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	accs := account.NewStore(pool, 0)
	a, err := accs.Register(ctx, "mine1", "password-123")
	if err != nil {
		t.Fatal(err)
	}
	b, err := accs.Register(ctx, "other1", "password-123")
	if err != nil {
		t.Fatal(err)
	}
	return character.NewStore(pool), a.ID, b.ID
}

func lines(out *bytes.Buffer) []string {
	if out.Len() == 0 {
		return nil
	}
	return strings.Split(strings.TrimSuffix(out.String(), "\n"), "\n")
}

func sameJSON(t *testing.T, a, b string) bool {
	t.Helper()
	var x, y any
	if err := json.Unmarshal([]byte(a), &x); err != nil {
		t.Fatalf("%q: %v", a, err)
	}
	if err := json.Unmarshal([]byte(b), &y); err != nil {
		t.Fatalf("%q: %v", b, err)
	}
	return reflect.DeepEqual(x, y)
}

// CoA / C | Y | B

func TestImportCarriesAWholeCharacter(t *testing.T) {
	ctx := context.Background()
	store, me, _ := importSetup(t)

	// Parsed the way charimport does it: the account is not known yet.
	info := cat(customization(mapEntry("CustomizationDataKey", looks)), classField(2))
	in, err := json.Marshal([]any{map[string]any{
		"character_id": hexID("a"), "character_name": "cyb", "class_name": "Bard", "archetype_id": 17747,
		"level": 4, "gender_name": "Male", "create_info_hex": hex.EncodeToString(info),
	}})
	if err != nil {
		t.Fatal(err)
	}
	chars, err := Parse(bytes.NewReader(in), 0)
	if err != nil {
		t.Fatal(err)
	}

	var out bytes.Buffer
	sum, err := Import(ctx, &out, store, me, chars)
	if err != nil {
		t.Fatal(err)
	}
	if want := (Summary{Imported: 1}); sum != want {
		t.Errorf("summary %+v, want %+v", sum, want)
	}
	if want := []string{fmt.Sprintf("imported cyb (Bard, %s) for account %d", hexID("a"), me)}; !reflect.DeepEqual(lines(&out), want) {
		t.Errorf("output %q, want %q", lines(&out), want)
	}

	got, err := store.Get(ctx, hexID("a"))
	if err != nil {
		t.Fatal(err)
	}
	if got.AccountID != me || got.Name != "cyb" || got.ClassName != "Bard" || got.ArchetypeID != 17747 ||
		got.Level != 4 || got.RaceID != 0 || got.GenderID != 0 || got.ClassID != 2 || !bytes.Equal(got.CreateInfo, info) ||
		!sameJSON(t, got.AppearanceJSON, looks) {
		t.Errorf("stored %+v", got)
	}
	if list, err := store.List(ctx, me); err != nil || len(list) != 1 {
		t.Errorf("List: %d characters, %v", len(list), err)
	}
}

func TestImportOwnsWhatItImports(t *testing.T) {
	// The account is the one Import is given, whatever the records say.
	ctx := context.Background()
	store, me, other := importSetup(t)
	var out bytes.Buffer
	sum, err := Import(ctx, &out, store, me, []character.New{
		{ID: hexID("1"), AccountID: 0, Name: "Alpha", ClassName: "Bard"},
		{ID: hexID("2"), AccountID: other, Name: "Bravo", ClassName: "Mage"},
	})
	if err != nil || sum != (Summary{Imported: 2}) {
		t.Fatalf("%+v, %v\n%s", sum, err, out.String())
	}
	if list, err := store.List(ctx, me); err != nil || len(list) != 2 {
		t.Errorf("List(me): %d characters, %v", len(list), err)
	}
	if list, err := store.List(ctx, other); err != nil || len(list) != 0 {
		t.Errorf("List(other): %d characters, %v", len(list), err)
	}
}

func TestImportSkipsWhatIsAlreadyThere(t *testing.T) {
	ctx := context.Background()
	store, me, _ := importSetup(t)
	chars := []character.New{{ID: hexID("1"), Name: "Alpha", ClassName: "Bard", CreateInfo: []byte{1, 2, 3}}}
	if _, err := Import(ctx, &bytes.Buffer{}, store, me, chars); err != nil {
		t.Fatal(err)
	}

	// A rerun changes nothing, even when the file's data has moved on.
	chars[0].ClassName = "Mage"
	chars[0].Level = 30
	var out bytes.Buffer
	sum, err := Import(ctx, &out, store, me, chars)
	if err != nil {
		t.Fatal(err)
	}
	if want := (Summary{AlreadyPresent: 1}); sum != want {
		t.Errorf("summary %+v, want %+v", sum, want)
	}
	if want := []string{fmt.Sprintf("skip Alpha (%s): already imported", hexID("1"))}; !reflect.DeepEqual(lines(&out), want) {
		t.Errorf("output %q, want %q", lines(&out), want)
	}
	got, err := store.Get(ctx, hexID("1"))
	if err != nil || got.ClassName != "Bard" || got.Level != 1 || string(got.CreateInfo) != "\x01\x02\x03" {
		t.Errorf("stored %+v, %v", got, err)
	}
}

func TestImportLeavesAnotherAccountsIDAlone(t *testing.T) {
	ctx := context.Background()
	store, me, other := importSetup(t)
	if _, err := store.Create(ctx, character.New{ID: hexID("9"), AccountID: other, Name: "Theirs", ClassName: "Bard"}); err != nil {
		t.Fatal(err)
	}

	var out bytes.Buffer
	sum, err := Import(ctx, &out, store, me, []character.New{{ID: hexID("9"), Name: "Mine", ClassName: "Mage"}})
	if err != nil {
		t.Fatal(err)
	}
	if want := (Summary{Conflicts: 1}); sum != want {
		t.Errorf("summary %+v, want %+v", sum, want)
	}
	if want := []string{fmt.Sprintf("CONFLICT Mine (%s): id belongs to another account", hexID("9"))}; !reflect.DeepEqual(lines(&out), want) {
		t.Errorf("output %q, want %q", lines(&out), want)
	}
	got, err := store.Get(ctx, hexID("9"))
	if err != nil || got.AccountID != other || got.Name != "Theirs" || got.ClassName != "Bard" {
		t.Errorf("their character was touched: %+v, %v", got, err)
	}
	if list, err := store.List(ctx, me); err != nil || len(list) != 0 {
		t.Errorf("List(me): %d characters, %v", len(list), err)
	}
}

func TestImportReportsANameAnotherCharacterHolds(t *testing.T) {
	ctx := context.Background()
	store, me, other := importSetup(t)
	if _, err := store.Create(ctx, character.New{ID: hexID("1"), AccountID: other, Name: "Alpha", ClassName: "Bard"}); err != nil {
		t.Fatal(err)
	}

	var out bytes.Buffer
	// The names compare case-insensitively.
	sum, err := Import(ctx, &out, store, me, []character.New{{ID: hexID("2"), Name: "ALPHA", ClassName: "Mage"}})
	if err != nil {
		t.Fatal(err)
	}
	if want := (Summary{Conflicts: 1}); sum != want {
		t.Errorf("summary %+v, want %+v", sum, want)
	}
	if want := []string{fmt.Sprintf("CONFLICT ALPHA (%s): character name taken", hexID("2"))}; !reflect.DeepEqual(lines(&out), want) {
		t.Errorf("output %q, want %q", lines(&out), want)
	}
	if _, err := store.Get(ctx, hexID("2")); !errors.Is(err, character.ErrNotFound) {
		t.Errorf("the conflicting record was stored: %v", err)
	}
}

// C Y B + CoA

func TestImportAnIDTwiceInTheFileIsAConflict(t *testing.T) {
	ctx := context.Background()
	store, me, _ := importSetup(t)
	var out bytes.Buffer
	sum, err := Import(ctx, &out, store, me, []character.New{
		{ID: hexID("3"), Name: "Gamma", ClassName: "Mage"},
		{ID: hexID("3"), Name: "Gammb", ClassName: "Mage"},
	})
	if err != nil {
		t.Fatal(err)
	}
	if want := (Summary{Imported: 1, Conflicts: 1}); sum != want {
		t.Errorf("summary %+v, want %+v", sum, want)
	}
	want := []string{
		fmt.Sprintf("imported Gamma (Mage, %s) for account %d", hexID("3"), me),
		fmt.Sprintf("CONFLICT Gammb (%s): duplicate id in file", hexID("3")),
	}
	if !reflect.DeepEqual(lines(&out), want) {
		t.Errorf("output %q, want %q", lines(&out), want)
	}
	got, err := store.Get(ctx, hexID("3"))
	if err != nil || got.Name != "Gamma" {
		t.Errorf("stored %+v, %v", got, err)
	}
}

func TestImportSkipsAnInvalidRecordAndCarriesOn(t *testing.T) {
	ctx := context.Background()
	store, me, _ := importSetup(t)
	var out bytes.Buffer
	sum, err := Import(ctx, &out, store, me, []character.New{
		{ID: hexID("2"), Name: "Bad Name", ClassName: "Bard"},
		{ID: hexID("3"), Name: "Gamma", ClassName: "Mage"},
	})
	if err != nil {
		t.Fatal(err)
	}
	if want := (Summary{Imported: 1, Invalid: 1}); sum != want {
		t.Errorf("summary %+v, want %+v", sum, want)
	}
	want := []string{
		fmt.Sprintf("INVALID Bad Name (%s): %v", hexID("2"), character.ValidateName("Bad Name")),
		fmt.Sprintf("imported Gamma (Mage, %s) for account %d", hexID("3"), me),
	}
	if !reflect.DeepEqual(lines(&out), want) {
		t.Errorf("output %q, want %q", lines(&out), want)
	}
	if list, err := store.List(ctx, me); err != nil || len(list) != 1 || list[0].Name != "Gamma" {
		t.Errorf("List: %+v, %v", list, err)
	}
}

func TestImportSummaryAndOutputForAMixedFile(t *testing.T) {
	ctx := context.Background()
	store, me, other := importSetup(t)
	if _, err := store.Create(ctx, character.New{ID: hexID("9"), AccountID: other, Name: "Theirs", ClassName: "Bard"}); err != nil {
		t.Fatal(err)
	}
	chars := []character.New{
		{ID: hexID("1"), Name: "Alpha", ClassName: "Bard", CreateInfo: []byte{1, 2, 3}},
		{ID: hexID("2"), Name: "Bad Name", ClassName: "Bard"}, // invalid: must not block the rest
		{ID: hexID("3"), Name: "Gamma", ClassName: "Mage"},
		{ID: hexID("3"), Name: "Gammb", ClassName: "Mage"}, // the same id twice in the file
		{ID: hexID("4"), Name: "ALPHA", ClassName: "Mage"}, // the name is taken, case-insensitively
		{ID: hexID("9"), Name: "Mine", ClassName: "Mage"},  // an id owned by someone else
	}

	var out bytes.Buffer
	sum, err := Import(ctx, &out, store, me, chars)
	if err != nil {
		t.Fatal(err)
	}
	if want := (Summary{Imported: 2, Conflicts: 3, Invalid: 1}); sum != want {
		t.Errorf("summary %+v, want %+v", sum, want)
	}
	want := []string{
		fmt.Sprintf("imported Alpha (Bard, %s) for account %d", hexID("1"), me),
		fmt.Sprintf("INVALID Bad Name (%s): %v", hexID("2"), character.ValidateName("Bad Name")),
		fmt.Sprintf("imported Gamma (Mage, %s) for account %d", hexID("3"), me),
		fmt.Sprintf("CONFLICT Gammb (%s): duplicate id in file", hexID("3")),
		fmt.Sprintf("CONFLICT ALPHA (%s): character name taken", hexID("4")),
		fmt.Sprintf("CONFLICT Mine (%s): id belongs to another account", hexID("9")),
	}
	if !reflect.DeepEqual(lines(&out), want) {
		t.Errorf("output:\n%s\nwant:\n%s", strings.Join(lines(&out), "\n"), strings.Join(want, "\n"))
	}
	list, err := store.List(ctx, me)
	if err != nil || len(list) != 2 || list[0].Name != "Alpha" || list[1].Name != "Gamma" || string(list[0].CreateInfo) != "\x01\x02\x03" {
		t.Errorf("stored %+v, %v", list, err)
	}

	// The rerun finds the two it carried over, and the same trouble again.
	out.Reset()
	sum, err = Import(ctx, &out, store, me, chars)
	if err != nil {
		t.Fatal(err)
	}
	if want := (Summary{AlreadyPresent: 2, Conflicts: 3, Invalid: 1}); sum != want {
		t.Errorf("second run %+v, want %+v\n%s", sum, want, out.String())
	}
}

// C_Y_B.CoA

func TestImportOfNothing(t *testing.T) {
	ctx := context.Background()
	store, me, _ := importSetup(t)
	var out bytes.Buffer
	sum, err := Import(ctx, &out, store, me, nil)
	if err != nil || sum != (Summary{}) || out.Len() != 0 {
		t.Errorf("%+v, %v, output %q", sum, err, out.String())
	}
}

// fakeStore drives the paths a healthy database never takes.
type fakeStore struct {
	get     func(id string) (character.Character, error)
	create  func(n character.New) (character.Character, error)
	gets    []string
	created []character.New
}

func (f *fakeStore) Get(_ context.Context, id string) (character.Character, error) {
	f.gets = append(f.gets, id)
	return f.get(id)
}

func (f *fakeStore) Create(_ context.Context, n character.New) (character.Character, error) {
	f.created = append(f.created, n)
	return f.create(n)
}

func notFound(string) (character.Character, error) {
	return character.Character{}, character.ErrNotFound
}

func created(n character.New) (character.Character, error) {
	return character.Character{ID: n.ID, AccountID: n.AccountID, Name: n.Name, ClassName: n.ClassName}, nil
}

func TestImportStopsOnAnInfrastructureError(t *testing.T) {
	boom := errors.New("connection reset")
	three := []character.New{
		{ID: hexID("1"), Name: "Alpha"}, {ID: hexID("2"), Name: "Bravo"}, {ID: hexID("3"), Name: "Charlie"},
	}
	for name, tc := range map[string]struct {
		store   *fakeStore
		wantSum Summary
		gets    int
	}{
		"Get fails": {&fakeStore{
			get: func(id string) (character.Character, error) {
				if id == hexID("2") {
					return character.Character{}, boom
				}
				return notFound(id)
			},
			create: created,
		}, Summary{Imported: 1}, 2},
		"Get fails with a wrapped context error": {&fakeStore{
			get: func(id string) (character.Character, error) {
				if id == hexID("2") {
					return character.Character{}, fmt.Errorf("query: %w", context.Canceled)
				}
				return notFound(id)
			},
			create: created,
		}, Summary{Imported: 1}, 2},
		"Create fails": {&fakeStore{
			get: notFound,
			create: func(n character.New) (character.Character, error) {
				if n.ID == hexID("2") {
					return character.Character{}, boom
				}
				return created(n)
			},
		}, Summary{Imported: 1}, 2},
	} {
		var out bytes.Buffer
		sum, err := Import(context.Background(), &out, tc.store, 5, three)
		if err == nil {
			t.Errorf("%s: no error", name)
			continue
		}
		if !strings.Contains(err.Error(), "Bravo") {
			t.Errorf("%s: error %q should name the character", name, err)
		}
		if !errors.Is(err, boom) && !errors.Is(err, context.Canceled) {
			t.Errorf("%s: error %q lost its cause", name, err)
		}
		if sum != tc.wantSum {
			t.Errorf("%s: summary %+v, want the work done so far %+v", name, sum, tc.wantSum)
		}
		if len(tc.store.gets) != tc.gets {
			t.Errorf("%s: %d Gets, want %d (nothing after the failure)", name, len(tc.store.gets), tc.gets)
		}
		if strings.Contains(out.String(), "Charlie") {
			t.Errorf("%s: went on past the failure:\n%s", name, out.String())
		}
	}
}

func TestImportClassifiesWrappedSentinels(t *testing.T) {
	// The store's sentinels may arrive wrapped, and ErrIDTaken is what a race
	// between Get and Create looks like.
	for name, tc := range map[string]struct {
		getErr, createErr error
		want              Summary
		created           int
	}{
		"ErrNotFound wrapped from Get": {fmt.Errorf("lookup: %w", character.ErrNotFound), nil, Summary{Imported: 1}, 1},
		"ErrIDTaken":                   {character.ErrNotFound, character.ErrIDTaken, Summary{Conflicts: 1}, 1},
		"ErrNameTaken":                 {character.ErrNotFound, character.ErrNameTaken, Summary{Conflicts: 1}, 1},
		"ErrInvalid wrapped":           {character.ErrNotFound, fmt.Errorf("%w: no account 5", character.ErrInvalid), Summary{Invalid: 1}, 1},
	} {
		f := &fakeStore{
			get: func(string) (character.Character, error) { return character.Character{}, tc.getErr },
			create: func(n character.New) (character.Character, error) {
				if tc.createErr != nil {
					return character.Character{}, tc.createErr
				}
				return created(n)
			},
		}
		var out bytes.Buffer
		sum, err := Import(context.Background(), &out, f, 5, []character.New{{ID: hexID("1"), Name: "Alpha"}})
		if err != nil || sum != tc.want || len(f.created) != tc.created {
			t.Errorf("%s: %+v, %v, %d Creates; want %+v, %d\n%s", name, sum, err, len(f.created), tc.want, tc.created, out.String())
		}
	}
}

// (C.O.A + C:Y:B)

func TestImportStampsTheAccountBeforeCreate(t *testing.T) {
	f := &fakeStore{get: notFound, create: created}
	_, err := Import(context.Background(), &bytes.Buffer{}, f, 5, []character.New{{ID: hexID("1"), Name: "Alpha", AccountID: 99}})
	if err != nil || len(f.created) != 1 || f.created[0].AccountID != 5 {
		t.Errorf("created %+v, %v", f.created, err)
	}
}

func TestImportAnotherAccountIsNotAttempted(t *testing.T) {
	f := &fakeStore{
		get:    func(id string) (character.Character, error) { return character.Character{ID: id, AccountID: 6}, nil },
		create: created,
	}
	sum, err := Import(context.Background(), &bytes.Buffer{}, f, 5, []character.New{{ID: hexID("1"), Name: "Alpha"}})
	if err != nil || sum != (Summary{Conflicts: 1}) || len(f.created) != 0 {
		t.Errorf("%+v, %v, %d Creates", sum, err, len(f.created))
	}
}

func TestImportRecordsWithoutAnIDAreNotDeduplicated(t *testing.T) {
	// Parse never yields an empty id, but Import takes any records: the store
	// makes an id for each, so two of them are two characters, not a repeat.
	f := &fakeStore{get: notFound, create: created}
	sum, err := Import(context.Background(), &bytes.Buffer{}, f, 5, []character.New{{Name: "Alpha"}, {Name: "Bravo"}})
	if err != nil || sum != (Summary{Imported: 2}) || len(f.created) != 2 {
		t.Errorf("%+v, %v, %d Creates", sum, err, len(f.created))
	}
}
