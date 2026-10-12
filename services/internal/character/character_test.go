// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"log/slog"
	"math"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgconn"

	"aocrop/services/internal/account"
	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"
)

func TestValidateName(t *testing.T) {
	for _, tc := range []struct {
		name string
		ok   bool
	}{
		{"cyb", true}, {"RandomChar", true}, {"Abc123", true},
		{"ab", false}, {"1abc", false}, {"has space", false}, {"toolongnamexxxxxxxxxx", false},
	} {
		if err := ValidateName(tc.name); (err == nil) != tc.ok {
			t.Errorf("ValidateName(%q) = %v, want ok=%v", tc.name, err, tc.ok)
		}
	}
}

func setup(t *testing.T) (*Store, int64) {
	t.Helper()
	pool := dbtest.Pool(t)
	ctx := context.Background()
	if err := db.Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	acc, err := account.NewStore(pool, 0).Register(ctx, "owner", "secret1")
	if err != nil {
		t.Fatal(err)
	}
	return NewStore(pool), acc.ID
}

// [C-O-A :: Cyb]

func TestCreateListGetSave(t *testing.T) {
	s, accountID := setup(t)
	ctx := context.Background()

	c, err := s.Create(ctx, New{
		AccountID: accountID, Name: "cyb", RaceID: 0, GenderID: 0,
		ArchetypeID: 17747, ClassName: "Bard", CreateInfo: []byte{1, 2, 3},
	})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	if len(c.ID) != 32 || c.Level != 1 || c.AppearanceJSON != "{}" || c.Position != nil {
		t.Fatalf("Create returned %+v", c)
	}
	if _, err := s.Create(ctx, New{AccountID: accountID, Name: "CYB", ArchetypeID: 1, ClassName: "Fighter"}); !errors.Is(err, ErrNameTaken) {
		t.Errorf("duplicate name: %v, want ErrNameTaken", err)
	}

	list, err := s.List(ctx, accountID)
	if err != nil || len(list) != 1 || list[0].ID != c.ID {
		t.Fatalf("List: %+v %v", list, err)
	}

	got, err := s.Get(ctx, c.ID)
	if err != nil || got.Name != "cyb" || string(got.CreateInfo) != "\x01\x02\x03" {
		t.Fatalf("Get: %+v %v", got, err)
	}
	if _, err := s.Get(ctx, "00000000000000000000000000000000"); !errors.Is(err, ErrNotFound) {
		t.Errorf("Get unknown: %v", err)
	}

	saved, err := s.SaveState(ctx, c.ID, State{
		Position: &Position{X: -777664.5, Y: 616578.7, Z: 15952.05, Yaw: 90}, LastRegion: "Riverlands", Level: 2,
	})
	if err != nil || saved.Position == nil || saved.Position.X != -777664.5 || saved.LastRegion != "Riverlands" || saved.Level != 2 {
		t.Fatalf("SaveState: %+v %v", saved, err)
	}
	if _, err := s.SaveState(ctx, "ffffffffffffffffffffffffffffffff", State{}); !errors.Is(err, ErrNotFound) {
		t.Errorf("SaveState unknown: %v", err)
	}
}

func TestCreateWithGivenID(t *testing.T) {
	s, accountID := setup(t)
	ctx := context.Background()
	id := "bbec5c86eb101fe5f38c87f0fa22e7d0"
	c, err := s.Create(ctx, New{ID: id, AccountID: accountID, Name: "cyb", ArchetypeID: 17747, ClassName: "Bard"})
	if err != nil || c.ID != id {
		t.Fatalf("Create with id: %+v %v", c, err)
	}
	if _, err := s.Create(ctx, New{ID: id, AccountID: accountID, Name: "other", ClassName: "Bard"}); !errors.Is(err, ErrIDTaken) {
		t.Errorf("same id, other name: %v, want ErrIDTaken", err)
	}
	if _, err := s.Create(ctx, New{ID: "NOT-HEX", AccountID: accountID, Name: "other", ClassName: "Bard"}); !errors.Is(err, ErrInvalid) {
		t.Errorf("bad id: %v, want ErrInvalid", err)
	}
}

// Input the database would reject (or, for NaN, silently store) is the
// caller's mistake: ErrInvalid, never a raw SQL error.
func TestRejectsBadInput(t *testing.T) {
	s, accountID := setup(t)
	ctx := context.Background()
	for _, tc := range []struct {
		name string
		n    New
	}{
		{"no account", New{Name: "cyb", ClassName: "Bard"}},
		{"unknown account", New{AccountID: accountID + 1000, Name: "cyb", ClassName: "Bard"}},
		{"negative level", New{AccountID: accountID, Name: "cyb", ClassName: "Bard", Level: -1}},
		{"level above the game's maximum", New{AccountID: accountID, Name: "cyb", ClassName: "Bard", Level: MaxLevel + 1}},
		{"appearance not JSON", New{AccountID: accountID, Name: "cyb", ClassName: "Bard", AppearanceJSON: "not json"}},
		{"appearance not an object", New{AccountID: accountID, Name: "cyb", ClassName: "Bard", AppearanceJSON: "[1]"}},
		{"appearance null", New{AccountID: accountID, Name: "cyb", ClassName: "Bard", AppearanceJSON: "null"}},
		{"appearance with NUL", New{AccountID: accountID, Name: "cyb", ClassName: "Bard", AppearanceJSON: `{"a": "\u0000"}`}},
		{"class with NUL", New{AccountID: accountID, Name: "cyb", ClassName: "Ba\x00rd"}},
	} {
		if _, err := s.Create(ctx, tc.n); !errors.Is(err, ErrInvalid) {
			t.Errorf("Create %s: %v, want ErrInvalid", tc.name, err)
		}
	}

	c, err := s.Create(ctx, New{AccountID: accountID, Name: "cyb", ClassName: "Bard", AppearanceJSON: `{"hair": 3}`})
	if err != nil {
		t.Fatal(err)
	}
	nan := math.NaN()
	for _, tc := range []struct {
		name string
		st   State
	}{
		{"negative level", State{Level: -3}},
		{"level above the game's maximum", State{Level: MaxLevel + 1}},
		{"NaN x", State{Position: &Position{X: nan}}},
		{"infinite z", State{Position: &Position{Z: math.Inf(1)}}},
		{"NaN yaw", State{Position: &Position{Yaw: float32(nan)}}},
		{"region with NUL", State{LastRegion: "River\x00lands"}},
	} {
		if _, err := s.SaveState(ctx, c.ID, tc.st); !errors.Is(err, ErrInvalid) {
			t.Errorf("SaveState %s: %v, want ErrInvalid", tc.name, err)
		}
	}
}

func TestValidateNameBoundaries(t *testing.T) {
	for name, ok := range map[string]bool{
		"abc": true, strings.Repeat("a", 20): true, "a1b": true,
		strings.Repeat("a", 21): false, "a_b": false, "abc-d": false, "abc\n": false,
		"\u00c9mile": false, "": false, "abc ": false, " abc": false,
	} {
		if err := ValidateName(name); (err == nil) != ok {
			t.Errorf("ValidateName(%q) = %v, want ok=%v", name, err, ok)
		}
	}
}

// -- C_Y_B . CoA --

func TestCreateRejectsInvalidName(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	for _, name := range []string{"", "ab", "has space", "1abc", "a_b"} {
		if _, err := s.Create(ctx, New{AccountID: acc, Name: name, ClassName: "Bard"}); !errors.Is(err, ErrInvalid) {
			t.Errorf("Create name %q: %v, want ErrInvalid", name, err)
		}
	}
	if l, err := s.List(ctx, acc); err != nil || len(l) != 0 {
		t.Errorf("nothing should have been stored: %+v %v", l, err)
	}
}

// Every column written by Create must come back from Create, Get and List.
func TestCreateRoundTripsEveryColumn(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	id := "0123456789abcdef0123456789abcdef"
	want := Character{ID: id, AccountID: acc, Name: "Zed", RaceID: 3, GenderID: 1, ClassID: 6, ArchetypeID: 17747,
		ClassName: "Bard", AppearanceJSON: `{"a": [3], "b": 1}`, CreateInfo: []byte{9, 8}, Level: 7}
	c, err := s.Create(ctx, New{ID: id, AccountID: acc, Name: "Zed", RaceID: 3, GenderID: 1, ClassID: 6, ArchetypeID: 17747,
		ClassName: "Bard", AppearanceJSON: `{"b":1,"a":[3]}`, CreateInfo: []byte{9, 8}, Level: 7})
	if err != nil {
		t.Fatal(err)
	}
	got, err := s.Get(ctx, id)
	if err != nil {
		t.Fatal(err)
	}
	list, err := s.List(ctx, acc)
	if err != nil || len(list) != 1 {
		t.Fatalf("List: %+v %v", list, err)
	}
	for label, g := range map[string]Character{"Create": c, "Get": got, "List": list[0]} {
		if g.CreatedAt.IsZero() {
			t.Errorf("%s: CreatedAt is zero", label)
		}
		g.CreatedAt = time.Time{} // checked above; everything else must match exactly
		if !reflect.DeepEqual(g, want) {
			t.Errorf("%s = %+v, want %+v", label, g, want)
		}
	}
}

// Race, gender and class have columns of their own (plan B1): every race 0-8,
// gender 0-1 and class 0-8 round-trips, a class of 0 is stored as NULL ("not
// derived") and read back as 0, and the legacy columns race_id and gender_id,
// which held the class and the ids 1 and 2, are no longer written.
func TestRaceGenderAndClassHaveTheirOwnColumns(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	n := 0
	for race := int32(0); race <= 8; race++ {
		for gender := int32(0); gender <= 1; gender++ {
			class := (race + gender) % 9 // 0-8: covers the no-class case too
			n++
			c, err := s.Create(ctx, New{AccountID: acc, Name: fmt.Sprintf("char%d", n), RaceID: race, GenderID: gender,
				ClassID: class, ArchetypeID: 17747, ClassName: "Bard"})
			if err != nil {
				t.Fatalf("race %d gender %d class %d: %v", race, gender, class, err)
			}
			got, err := s.Get(ctx, c.ID)
			if err != nil || got.RaceID != race || got.GenderID != gender || got.ClassID != class {
				t.Errorf("race %d gender %d class %d came back as %d %d %d (%v)", race, gender, class, got.RaceID, got.GenderID, got.ClassID, err)
			}
			var raceCol, genderCol, classCol *int32
			var legacyRace, legacyGender int32
			if err := s.pool.QueryRow(ctx, "SELECT race, gender, class_id, race_id, gender_id FROM characters WHERE id = $1", c.ID).
				Scan(&raceCol, &genderCol, &classCol, &legacyRace, &legacyGender); err != nil {
				t.Fatal(err)
			}
			if *raceCol != race || *genderCol != gender || (class == 0) != (classCol == nil) || (classCol != nil && *classCol != class) {
				t.Errorf("columns race %v gender %v class_id %v for %d %d %d", raceCol, genderCol, classCol, race, gender, class)
			}
			if legacyRace != 0 || legacyGender != 0 {
				t.Errorf("the legacy columns race_id %d gender_id %d were written", legacyRace, legacyGender)
			}
		}
	}
}

// A row the migration could not derive race, gender or class for holds NULL,
// which reads as 0 (Kaelar, Male, no class).
func TestUnderivedRowsReadAsZero(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	const id = "0123456789abcdef0123456789abcdef"
	// each such character is reported once, by id, however often it is read
	loggedUnderived.Clear()
	var logs bytes.Buffer
	saved := slog.Default()
	slog.SetDefault(slog.New(slog.NewTextHandler(&logs, nil)))
	defer slog.SetDefault(saved)
	defer func() {
		if n := strings.Count(logs.String(), "character_id="+id); n != 1 {
			t.Errorf("the NULL race and gender were reported %d times, want once:\n%s", n, logs.String())
		}
		if !strings.Contains(logs.String(), "level=WARN") || !strings.Contains(logs.String(), "race or gender is NULL") {
			t.Errorf("the report is not a warning about the NULL race or gender:\n%s", logs.String())
		}
	}()
	if _, err := s.pool.Exec(ctx, `INSERT INTO characters (id, account_id, name, race_id, gender_id, archetype_id, class_name)
		VALUES ($1, $2, 'Old', 2, 1, 17747, 'Bard')`, id, acc); err != nil {
		t.Fatal(err)
	}
	got, err := s.Get(ctx, id)
	if err != nil || got.RaceID != 0 || got.GenderID != 0 || got.ClassID != 0 || got.ArchetypeID != 17747 {
		t.Errorf("Get = %+v, %v; want race, gender and class 0 and the archetype as stored", got, err)
	}
	list, err := s.List(ctx, acc)
	if err != nil || len(list) != 1 || list[0].RaceID != 0 || list[0].ClassID != 0 {
		t.Errorf("List = %+v, %v", list, err)
	}
	if got, err := s.SaveState(ctx, id, State{Level: 3}); err != nil || got.Level != 3 || got.RaceID != 0 || got.ClassID != 0 {
		t.Errorf("SaveState = %+v, %v", got, err)
	}
	// a character whose columns are all set is not reported
	made, err := s.Create(ctx, New{AccountID: acc, Name: "Derived", RaceID: 0, GenderID: 0, ClassID: 2, ClassName: "Bard"})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.Get(ctx, made.ID); err != nil {
		t.Fatal(err)
	}
	if strings.Contains(logs.String(), made.ID) {
		t.Errorf("a character with race and gender set was reported:\n%s", logs.String())
	}
}

// (C O A) (C:Y:B)

func TestRejectsAnIdThatIsNoEnumValue(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	for name, n := range map[string]New{
		"race 9":    {RaceID: 9},
		"race -1":   {RaceID: -1},
		"gender 2":  {GenderID: 2},
		"gender -1": {GenderID: -1},
		"class 9":   {ClassID: 9},
		"class -1":  {ClassID: -1},
	} {
		n.AccountID, n.Name, n.ClassName = acc, "Bad", "Bard"
		if _, err := s.Create(ctx, n); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: %v, want ErrInvalid", name, err)
		}
	}
	if l, err := s.List(ctx, acc); err != nil || len(l) != 0 {
		t.Errorf("nothing should have been stored: %+v %v", l, err)
	}
}

func TestGeneratedIDsDiffer(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	a, err := s.Create(ctx, New{AccountID: acc, Name: "one", ClassName: "Bard"})
	if err != nil {
		t.Fatal(err)
	}
	b, err := s.Create(ctx, New{AccountID: acc, Name: "two", ClassName: "Bard"})
	if err != nil {
		t.Fatalf("second generated id collided: %v", err)
	}
	if a.ID == b.ID {
		t.Errorf("two generated ids are equal: %s", a.ID)
	}
}

func TestCreateRejectsMalformedID(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	for _, id := range []string{
		"BBEC5C86EB101FE5F38C87F0FA22E7D0", strings.Repeat("a", 33), strings.Repeat("a", 31),
		"0123456789abcdef0123456789abcdef\n", "0123456789abcdef0123456789abcdeg",
	} {
		if _, err := s.Create(ctx, New{ID: id, AccountID: acc, Name: "idcase", ClassName: "Bard"}); !errors.Is(err, ErrInvalid) {
			t.Errorf("Create id %q: %v, want ErrInvalid", id, err)
		}
	}
}

func TestListScopedToAccountAndOrdered(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	other, err := account.NewStore(s.pool, 0).Register(ctx, "other", "secret1")
	if err != nil {
		t.Fatal(err)
	}
	var want []string
	var firstID string
	for i := 0; i < 8; i++ {
		name := fmt.Sprintf("mine%d", i)
		c, err := s.Create(ctx, New{AccountID: acc, Name: name, ClassName: "Bard"})
		if err != nil {
			t.Fatal(err)
		}
		if i == 0 {
			firstID = c.ID
		}
		want = append(want, name)
		if i == 3 {
			if _, err := s.Create(ctx, New{AccountID: other.ID, Name: "theirs", ClassName: "Bard"}); err != nil {
				t.Fatal(err)
			}
		}
	}
	// Without the (account_id, created_at) index the planner scans the heap in
	// physical order, and a save rewrites the row at the end of it: the order
	// must come from the query, not from luck.
	if _, err := s.pool.Exec(ctx, "DROP INDEX characters_account"); err != nil {
		t.Fatal(err)
	}
	if _, err := s.SaveState(ctx, firstID, State{Level: 2}); err != nil {
		t.Fatal(err)
	}
	names := func(l []Character) []string {
		var out []string
		for _, c := range l {
			out = append(out, c.Name)
		}
		return out
	}
	mine, err := s.List(ctx, acc)
	if err != nil || !reflect.DeepEqual(names(mine), want) {
		t.Errorf("List(acc) = %v %v, want %v in creation order", names(mine), err, want)
	}
	theirs, err := s.List(ctx, other.ID)
	if err != nil || !reflect.DeepEqual(names(theirs), []string{"theirs"}) {
		t.Errorf("List(other) = %v %v", names(theirs), err)
	}
	none, err := s.List(ctx, other.ID+1000)
	if err != nil || len(none) != 0 {
		t.Errorf("List(unknown account) = %v %v, want empty", names(none), err)
	}
}

func TestSaveStatePartialUpdatesKeepTheRest(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassName: "Bard", Level: 4})
	if err != nil {
		t.Fatal(err)
	}
	bystander, err := s.Create(ctx, New{AccountID: acc, Name: "other", ClassName: "Bard"})
	if err != nil {
		t.Fatal(err)
	}
	check := func(step string, st State, wantPos *Position, wantRegion string, wantLevel int32) {
		t.Helper()
		got, err := s.SaveState(ctx, c.ID, st)
		if err != nil {
			t.Fatalf("%s: %v", step, err)
		}
		again, err := s.Get(ctx, c.ID)
		if err != nil {
			t.Fatal(err)
		}
		for label, g := range map[string]Character{"returned": got, "stored": again} {
			if !reflect.DeepEqual(g.Position, wantPos) || g.LastRegion != wantRegion || g.Level != wantLevel {
				t.Errorf("%s (%s): pos=%+v region=%q level=%d; want pos=%+v region=%q level=%d",
					step, label, g.Position, g.LastRegion, g.Level, wantPos, wantRegion, wantLevel)
			}
		}
	}
	updatedAt := func() time.Time {
		t.Helper()
		var at time.Time
		if err := s.pool.QueryRow(ctx, "SELECT updated_at FROM characters WHERE id = $1", c.ID).Scan(&at); err != nil {
			t.Fatal(err)
		}
		return at
	}
	t0 := updatedAt()

	p1 := &Position{X: 1, Y: 2, Z: 3, Yaw: 4}
	check("nothing set on a fresh character", State{}, nil, "", 4)
	check("full", State{Position: p1, LastRegion: "A", Level: 5}, p1, "A", 5)
	check("empty state keeps all", State{}, p1, "A", 5)
	check("region only", State{LastRegion: "B"}, p1, "B", 5)
	check("level only", State{Level: 9}, p1, "B", 9)
	p2 := &Position{X: 5, Y: 6, Z: 7, Yaw: 8}
	check("position only", State{Position: p2}, p2, "B", 9)

	if t1 := updatedAt(); !t1.After(t0) {
		t.Errorf("updated_at did not advance: %v -> %v", t0, t1)
	}
	got, err := s.Get(ctx, bystander.ID)
	if err != nil || got.Position != nil || got.LastRegion != "" || got.Level != 1 {
		t.Errorf("another character changed: %+v %v", got, err)
	}
}

// [C.O.A ~ C | Y | B]

func TestSaveStateRejectsNonFinite(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassName: "Bard"})
	if err != nil {
		t.Fatal(err)
	}
	nan, inf := math.NaN(), math.Inf(1)
	for label, p := range map[string]Position{
		"NaN x": {X: nan}, "NaN y": {Y: nan}, "NaN z": {Z: nan}, "NaN yaw": {Yaw: float32(nan)},
		"+Inf x": {X: inf}, "-Inf x": {X: -inf}, "+Inf y": {Y: inf}, "-Inf y": {Y: -inf},
		"+Inf z": {Z: inf}, "-Inf z": {Z: -inf}, "+Inf yaw": {Yaw: float32(inf)}, "-Inf yaw": {Yaw: float32(-inf)},
	} {
		if _, err := s.SaveState(ctx, c.ID, State{Position: &p}); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: %v, want ErrInvalid", label, err)
		}
	}
	if got, err := s.Get(ctx, c.ID); err != nil || got.Position != nil {
		t.Errorf("a rejected save must store nothing: %+v %v", got.Position, err)
	}
}

// A row with x, y, z but no yaw (legacy import, manual SQL) reads as yaw 0.
func TestNullYawReadsAsZero(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassName: "Bard"})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.pool.Exec(ctx, "UPDATE characters SET pos_x = 1, pos_y = 2, pos_z = 3 WHERE id = $1", c.ID); err != nil {
		t.Fatal(err)
	}
	want := &Position{X: 1, Y: 2, Z: 3}
	got, err := s.Get(ctx, c.ID)
	if err != nil || !reflect.DeepEqual(got.Position, want) {
		t.Errorf("Get: %+v %v", got.Position, err)
	}
	list, err := s.List(ctx, acc)
	if err != nil || len(list) != 1 || !reflect.DeepEqual(list[0].Position, want) {
		t.Errorf("List: %+v %v", list, err)
	}
}

func TestInvalidDataOnlyMapsDataExceptions(t *testing.T) {
	for _, code := range []string{"22021", "22P02", "22003", "22P05"} {
		src := fmt.Errorf("insert: %w", &pgconn.PgError{Code: code, Message: "m"})
		if err := invalidData(src); !errors.Is(err, ErrInvalid) {
			t.Errorf("code %s: %v, want ErrInvalid", code, err)
		}
	}
	for _, code := range []string{"23514", "23503", "28P01", "08006", "57014", "42P01"} {
		src := fmt.Errorf("insert: %w", &pgconn.PgError{Code: code, Message: "m"})
		if err := invalidData(src); errors.Is(err, ErrInvalid) || err != src {
			t.Errorf("code %s: %v, want the original error untouched", code, err)
		}
	}
	if invalidData(nil) != nil {
		t.Error("invalidData(nil) != nil")
	}
}

// Text PostgreSQL refuses (or that Go's JSON accepts but jsonb does not) is
// the caller's mistake, not an Internal error.
func TestHostileInputIsInvalid(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	for _, in := range []string{`{"a":"\ud800"}`, `{"a":1e999999999}`, "{\"a\":\"\xff\"}", `{"\u0000":1}`} {
		if _, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassName: "Bard", AppearanceJSON: in}); !errors.Is(err, ErrInvalid) {
			t.Errorf("appearance %q: %v, want ErrInvalid", in, err)
		}
	}
	if _, err := s.Create(ctx, New{AccountID: acc, Name: "cyb", ClassName: "Ba\xffrd"}); !errors.Is(err, ErrInvalid) {
		t.Errorf("class with invalid UTF-8: %v, want ErrInvalid", err)
	}
}

// No character can have a malformed id, so Get, SaveState and Delete answer
// ErrNotFound for one without sending it to PostgreSQL (which rejects NUL
// and invalid UTF-8 with a raw error).
func TestMalformedIDsAreNotFound(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	// The two 32-byte ids have the right length and still are not ids: a guard
	// that only checked the length would send them on to PostgreSQL.
	for _, id := range []string{
		"", "NOT-HEX", "a\x00b", "\xff", "BBEC5C86EB101FE5F38C87F0FA22E7D0",
		strings.Repeat("\xff", 32), "0123456789abcdef0123456789abcde\x00",
	} {
		if _, err := s.Get(ctx, id); !errors.Is(err, ErrNotFound) {
			t.Errorf("Get(%q): %v, want ErrNotFound", id, err)
		}
		if _, err := s.SaveState(ctx, id, State{LastRegion: "A"}); !errors.Is(err, ErrNotFound) {
			t.Errorf("SaveState(%q): %v, want ErrNotFound", id, err)
		}
		if err := s.Delete(ctx, acc, id); !errors.Is(err, ErrNotFound) {
			t.Errorf("Delete(%q): %v, want ErrNotFound", id, err)
		}
	}
}

// == C | O | A | C-Y-B ==

func TestDeleteOnlyTheOwnersCharacter(t *testing.T) {
	s, accountID := setup(t)
	ctx := context.Background()
	other, err := account.NewStore(s.pool, 0).Register(ctx, "other", "secret1")
	if err != nil {
		t.Fatal(err)
	}
	c, err := s.Create(ctx, New{AccountID: accountID, Name: "cyb", ClassName: "Bard"})
	if err != nil {
		t.Fatal(err)
	}
	if err := s.Delete(ctx, other.ID, c.ID); !errors.Is(err, ErrNotFound) {
		t.Errorf("delete by another account: %v, want ErrNotFound", err)
	}
	if _, err := s.Get(ctx, c.ID); err != nil {
		t.Fatalf("character gone after a refused delete: %v", err)
	}
	if err := s.Delete(ctx, accountID, c.ID); err != nil {
		t.Fatalf("Delete: %v", err)
	}
	if _, err := s.Get(ctx, c.ID); !errors.Is(err, ErrNotFound) {
		t.Errorf("Get after delete: %v, want ErrNotFound", err)
	}
	if err := s.Delete(ctx, accountID, c.ID); !errors.Is(err, ErrNotFound) {
		t.Errorf("second delete: %v, want ErrNotFound", err)
	}
	if err := s.Delete(ctx, accountID, "NOT-HEX"); !errors.Is(err, ErrNotFound) {
		t.Errorf("malformed id: %v, want ErrNotFound", err)
	}
}

func TestCreatedAtIsRecorded(t *testing.T) {
	s, accountID := setup(t)
	ctx := context.Background()
	before := time.Now().Add(-time.Minute)
	c, err := s.Create(ctx, New{AccountID: accountID, Name: "cyb", ClassName: "Bard"})
	if err != nil {
		t.Fatal(err)
	}
	if c.CreatedAt.Before(before) || c.CreatedAt.After(time.Now().Add(time.Minute)) {
		t.Errorf("CreatedAt = %v, want about now", c.CreatedAt)
	}
	got, err := s.Get(ctx, c.ID)
	if err != nil || !got.CreatedAt.Equal(c.CreatedAt) {
		t.Errorf("Get: CreatedAt = %v (%v), want %v", got.CreatedAt, err, c.CreatedAt)
	}
}

// A database failure is neither success nor "not found", and the row stays.
func TestDeleteReportsADatabaseFailure(t *testing.T) {
	s, accountID := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: accountID, Name: "cyb", ClassName: "Bard"})
	if err != nil {
		t.Fatal(err)
	}
	canceled, cancel := context.WithCancel(ctx)
	cancel()
	if err := s.Delete(canceled, accountID, c.ID); err == nil || errors.Is(err, ErrNotFound) {
		t.Errorf("Delete under a canceled context: %v, want a database error", err)
	}
	if _, err := s.Get(ctx, c.ID); err != nil {
		t.Errorf("character gone after a failed delete: %v", err)
	}
}

// CreatedAt is the creation time, not the time of the last update.
func TestCreatedAtSurvivesASave(t *testing.T) {
	s, accountID := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: accountID, Name: "cyb", ClassName: "Bard"})
	if err != nil {
		t.Fatal(err)
	}
	time.Sleep(20 * time.Millisecond)
	saved, err := s.SaveState(ctx, c.ID, State{LastRegion: "Riverlands"})
	if err != nil {
		t.Fatal(err)
	}
	got, err := s.Get(ctx, c.ID)
	if err != nil {
		t.Fatal(err)
	}
	for label, at := range map[string]time.Time{"SaveState": saved.CreatedAt, "Get": got.CreatedAt} {
		if !at.Equal(c.CreatedAt) {
			t.Errorf("%s: CreatedAt = %v, want the creation time %v", label, at, c.CreatedAt)
		}
	}
}

// The stored value reaches the caller with the database's own precision.
func TestCreatedAtKeepsSubsecondPrecision(t *testing.T) {
	s, accountID := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: accountID, Name: "cyb", ClassName: "Bard"})
	if err != nil {
		t.Fatal(err)
	}
	var stored time.Time
	if err := s.pool.QueryRow(ctx, "SELECT created_at FROM characters WHERE id = $1", c.ID).Scan(&stored); err != nil {
		t.Fatal(err)
	}
	if !c.CreatedAt.Equal(stored) {
		t.Errorf("CreatedAt = %v, stored %v", c.CreatedAt, stored)
	}
}
