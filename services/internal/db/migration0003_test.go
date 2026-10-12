// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0003 (plan B1): race, gender and class in columns of their own,
// derived for the rows that exist, with the old values kept. It will run on the
// real database when the services start, so it is tested on rows shaped like
// the real ones (cyb's customization JSON, as the client wrote it) and on the
// rows it must leave alone.

import (
	"bytes"
	"context"
	"encoding/hex"
	"fmt"
	"log/slog"
	"os"
	"strings"
	"sync"
	"testing"
	"testing/fstest"
	"time"

	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"

	"aocrop/services/internal/createinfo"
	"aocrop/services/internal/dbtest"
)

const migration0003 = "migrations/0003_character_race_class.sql"

// upTo returns the embedded migrations of the versions below limit as a file
// system, so a test can stop the schema where the real database was.
func upTo(t *testing.T, limit int) fstest.MapFS {
	t.Helper()
	sub, err := migrations.ReadDir("migrations")
	if err != nil {
		t.Fatal(err)
	}
	out := fstest.MapFS{}
	for _, e := range sub {
		m := migrationName.FindStringSubmatch(e.Name())
		if m == nil {
			continue
		}
		var version int
		fmt.Sscanf(m[1], "%d", &version)
		if version < limit {
			data, err := migrations.ReadFile("migrations/" + e.Name())
			if err != nil {
				t.Fatal(err)
			}
			out[e.Name()] = &fstest.MapFile{Data: data}
		}
	}
	return out
}

// [C-Y-B x C | O | A]

// notices collects what PostgreSQL says outside results (RAISE ...).
type notices struct {
	mu   sync.Mutex
	list []string
}

func (n *notices) add(_ *pgconn.PgConn, notice *pgconn.Notice) {
	n.mu.Lock()
	defer n.mu.Unlock()
	n.list = append(n.list, notice.SeverityUnlocalized+": "+notice.Message)
}

func (n *notices) taking() []string {
	n.mu.Lock()
	defer n.mu.Unlock()
	out := n.list
	n.list = nil
	return out
}

// poolWithNotices is a second pool on the test's schema that records the notices.
func poolWithNotices(t *testing.T, base *pgxpool.Pool) (*pgxpool.Pool, *notices) {
	t.Helper()
	cfg := base.Config()
	got := &notices{}
	cfg.ConnConfig.OnNotice = got.add
	pool, err := pgxpool.NewWithConfig(context.Background(), cfg)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(pool.Close)
	return pool, got
}

// cybAppearance is the customization JSON of the real client's payload for cyb,
// a Kaelar male Bard: 41 members, 84 face morphs, 30 body sections.
func cybAppearance(t *testing.T) string {
	t.Helper()
	raw, err := os.ReadFile("../createinfo/testdata/client_create_info.hex")
	if err != nil {
		t.Fatal(err)
	}
	payload, err := hex.DecodeString(strings.TrimSpace(string(raw)))
	if err != nil {
		t.Fatal(err)
	}
	js := createinfo.Parse(payload).Appearance
	if js == "" {
		t.Fatal("no customization in the payload")
	}
	return js
}

// legacyRow is a row as the services wrote it before the migration: race_id
// held the class (the payload's field 4), gender_id 1 or 2, archetype_id the
// class's archetype record id.
type legacyRow struct {
	id, name              string
	raceID, genderID      int
	archetypeID           int
	className, appearance string
	// what the migration must make of it; nil: NULL (left, and logged)
	race, gender, class *int
	warns               []string // substrings of the warnings about it
}

func ptr(v int) *int { return &v }

func legacyRows(t *testing.T) []legacyRow {
	t.Helper()
	cyb := cybAppearance(t)
	named := func(race, gender, class string) string {
		return fmt.Sprintf(`{"race":%q,"gender":%q,"class":%q,"skinSet":1,"headHair":"6064630961557340182"}`, race, gender, class)
	}
	return []legacyRow{
		// cyb as it is in the real database: a Kaelar (0) male (0) Bard (2) whose race_id is 2, the class
		{id: "bbec5c86eb101fe5f38c87f0fa22e7d0", name: "cyb", raceID: 2, genderID: 1, archetypeID: 17747, className: "Bard",
			appearance: cyb, race: ptr(0), gender: ptr(0), class: ptr(2)},
		{id: "00000000000000000000000000000001", name: "DunirTank", raceID: 7, genderID: 2, archetypeID: 17754, className: "Tank",
			appearance: named("Dunir", "Female", "Tank"), race: ptr(6), gender: ptr(1), class: ptr(7)},
		{id: "00000000000000000000000000000002", name: "NikuaRanger", raceID: 1, genderID: 2, archetypeID: 17751, className: "Ranger",
			appearance: named("Nikua", "Female", "Ranger"), race: ptr(7), gender: ptr(1), class: ptr(1)},
		{id: "00000000000000000000000000000003", name: "VekMage", raceID: 8, genderID: 1, archetypeID: 17750, className: "Mage",
			appearance: named("Vek", "Male", "Mage"), race: ptr(2), gender: ptr(0), class: ptr(8)},
		{id: "00000000000000000000000000000004", name: "Empyrean", raceID: 3, genderID: 1, archetypeID: 17748, className: "Cleric",
			appearance: named("Empyrean", "Male", "Cleric"), race: ptr(4), gender: ptr(0), class: ptr(3)},
		// the class is class_name's; with no usable race_id nothing else is needed
		{id: "00000000000000000000000000000005", name: "NoField4", raceID: 0, genderID: 1, archetypeID: 17748, className: "Cleric",
			appearance: named("Vaelune", "Female", "Cleric"), race: ptr(1), gender: ptr(1), class: ptr(3)},
		// Tulnar cannot be created now, but a row that says it is one is derived as it says
		{id: "00000000000000000000000000000006", name: "Tulnar", raceID: 4, genderID: 1, archetypeID: 17749, className: "Fighter",
			appearance: named("Tulnar", "Male", "Fighter"), race: ptr(8), gender: ptr(0), class: ptr(4)},
		// rows whose JSON lacks the bytes: left, and logged one by one
		{id: "00000000000000000000000000000007", name: "EmptyJson", raceID: 2, genderID: 1, archetypeID: 17747, className: "Bard",
			appearance: `{}`, class: ptr(2), warns: []string{"EmptyJson", "race, gender"}},
		{id: "00000000000000000000000000000008", name: "RaceOnly", raceID: 4, genderID: 1, archetypeID: 17749, className: "Fighter",
			appearance: `{"race":"Pyrai"}`, race: ptr(5), class: ptr(4), warns: []string{"RaceOnly", "without: gender"}},
		{id: "00000000000000000000000000000009", name: "GenderOnly", raceID: 5, genderID: 2, archetypeID: 17752, className: "Rogue",
			appearance: `{"gender":"Female"}`, gender: ptr(1), class: ptr(5), warns: []string{"GenderOnly", "without: race"}},
		{id: "00000000000000000000000000000010", name: "NotNames", raceID: 6, genderID: 2, archetypeID: 17753, className: "Summoner",
			appearance: `{"race":6,"gender":"female"}`, class: ptr(6), warns: []string{"NotNames", "race, gender"}},
		{id: "00000000000000000000000000000011", name: "NotAnObject", raceID: 2, genderID: 1, archetypeID: 17747, className: "Bard",
			appearance: `[1,2]`, class: ptr(2), warns: []string{"NotAnObject"}},
		// no class at all: neither race_id nor class_name names one
		{id: "00000000000000000000000000000012", name: "NoClass", raceID: 9, genderID: 1, archetypeID: 0, className: "Paladin",
			appearance: named("Kaelar", "Male", "Paladin"), race: ptr(0), gender: ptr(0), warns: []string{"NoClass", "without: class"}},
		// race_id and class_name disagree: class_name (the class's own name, which names a class) wins, race_id is kept
		{id: "00000000000000000000000000000013", name: "Disagrees", raceID: 2, genderID: 1, archetypeID: 17750, className: "Mage",
			appearance: named("Kaelar", "Male", "Mage"), race: ptr(0), gender: ptr(0), class: ptr(8),
			warns: []string{"Disagrees", "has class 8 (from its class_name Mage) but its old race_id, which held the class, says 2 (race_id is kept)"}},
		// a class_name that names no class: the class is race_id (a class id), and nothing is said
		{id: "00000000000000000000000000000014", name: "NoName", raceID: 5, genderID: 2, archetypeID: 17752, className: "",
			appearance: named("Pyrai", "Female", ""), race: ptr(5), gender: ptr(1), class: ptr(5)},
	}
}

// CoA with C-Y-B

type snapshot struct {
	raceID, genderID, archetypeID, level int
	className, appearance, name          string
	race, gender, class                  *int
}

// rows reads every character, by id; before the migration the new columns do not
// exist and come back NULL.
func rows(t *testing.T, pool *pgxpool.Pool, migrated bool) map[string]snapshot {
	t.Helper()
	columns := "race, gender, class_id"
	if !migrated {
		columns = "NULL::int, NULL::int, NULL::int"
	}
	r, err := pool.Query(context.Background(), `SELECT id, name, race_id, gender_id, archetype_id, class_name, appearance::text, level, `+columns+`
		FROM characters`)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	out := map[string]snapshot{}
	for r.Next() {
		var id string
		var s snapshot
		if err := r.Scan(&id, &s.name, &s.raceID, &s.genderID, &s.archetypeID, &s.className, &s.appearance, &s.level, &s.race, &s.gender, &s.class); err != nil {
			t.Fatal(err)
		}
		out[id] = s
	}
	if err := r.Err(); err != nil {
		t.Fatal(err)
	}
	return out
}

func eq(a, b *int) bool { return (a == nil) == (b == nil) && (a == nil || *a == *b) }

func show(p *int) string {
	if p == nil {
		return "NULL"
	}
	return fmt.Sprint(*p)
}

// legacySetup builds the schema as it was before B1 (0001 and 0002) with the rows.
func legacySetup(t *testing.T) (*pgxpool.Pool, *notices, []legacyRow) {
	t.Helper()
	ctx := context.Background()
	base := dbtest.Pool(t)
	pool, got := poolWithNotices(t, base)
	if err := migrate(ctx, pool, upTo(t, 3)); err != nil {
		t.Fatal(err)
	}
	var account int64
	if err := pool.QueryRow(ctx, `INSERT INTO accounts (username, password_hash) VALUES ('Owner', 'x') RETURNING id`).Scan(&account); err != nil {
		t.Fatal(err)
	}
	legacy := legacyRows(t)
	for _, r := range legacy {
		if _, err := pool.Exec(ctx, `INSERT INTO characters (id, account_id, name, race_id, gender_id, archetype_id, class_name, appearance, create_info, level)
			VALUES ($1, $2, $3, $4, $5, $6, $7, $8::jsonb, $9, 4)`,
			r.id, account, r.name, r.raceID, r.genderID, r.archetypeID, r.className, r.appearance, []byte{1, 2, 3}); err != nil {
			t.Fatalf("%s: %v", r.name, err)
		}
	}
	got.taking()
	return pool, got, legacy
}

func TestMigration0003DerivesRaceGenderAndClassAndKeepsTheOldValues(t *testing.T) {
	ctx := context.Background()
	pool, notes, legacy := legacySetup(t)
	before := rows(t, pool, false)

	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	after := rows(t, pool, true)
	if len(after) != len(legacy) {
		t.Fatalf("%d rows after the migration, %d before", len(after), len(legacy))
	}
	for _, r := range legacy {
		a, b := after[r.id], before[r.id]
		if !eq(a.race, r.race) || !eq(a.gender, r.gender) || !eq(a.class, r.class) {
			t.Errorf("%s: race %s gender %s class %s, want %s %s %s", r.name, show(a.race), show(a.gender), show(a.class), show(r.race), show(r.gender), show(r.class))
		}
		// Nothing is lost: the old columns, and everything else, are as they were.
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil
		if a != b {
			t.Errorf("%s changed: %+v, was %+v", r.name, a, b)
		}
	}
	// cyb is a Kaelar male Bard, still: the class was in race_id (2).
	if c := after["bbec5c86eb101fe5f38c87f0fa22e7d0"]; *c.race != 0 || *c.gender != 0 || *c.class != 2 || c.raceID != 2 || c.genderID != 1 {
		t.Errorf("cyb: %+v", c)
	}

	// The log: one summary, and one warning per row that was left without something.
	list := notes.taking()
	summary := ""
	warns := map[string]string{}
	for _, n := range list {
		switch {
		case strings.HasPrefix(n, "NOTICE: migration 0003: "):
			summary += n
		case strings.HasPrefix(n, "WARNING: migration 0003: "):
			for _, r := range legacy {
				if strings.Contains(n, "("+"id "+r.id+")") {
					warns[r.name] += n
				}
			}
		}
	}
	if want := fmt.Sprintf("migration 0003: %d characters; race derived for 11, gender for 11, class for 14", len(legacy)); !strings.Contains(summary, want) {
		t.Errorf("summary %q does not contain %q", summary, want)
	}
	for _, r := range legacy {
		if len(r.warns) == 0 {
			if warns[r.name] != "" {
				t.Errorf("%s: unexpected warning %q", r.name, warns[r.name])
			}
			continue
		}
		for _, want := range r.warns {
			if !strings.Contains(warns[r.name], want) {
				t.Errorf("%s: warning %q does not contain %q", r.name, warns[r.name], want)
			}
		}
	}
}

// [C O A + Cyb]

// The migration can be run again: the data is the same, the old values are
// still there, and a value written since (by the services, which do not look at
// the JSON again) is never overwritten.
func TestMigration0003IsIdempotentAndNeverOverwrites(t *testing.T) {
	ctx := context.Background()
	pool, notes, _ := legacySetup(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	first := rows(t, pool, true)
	firstNotes := notes.taking()

	run := func() {
		t.Helper()
		sql, err := migrations.ReadFile(migration0003)
		if err != nil {
			t.Fatal(err)
		}
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		if _, err := tx.Exec(ctx, string(sql)); err != nil {
			t.Fatalf("the migration run again: %v", err)
		}
		if err := tx.Commit(ctx); err != nil {
			t.Fatal(err)
		}
	}
	run()
	run()
	second := rows(t, pool, true)
	for id, a := range first {
		b := second[id]
		if a.name != b.name || a.raceID != b.raceID || a.genderID != b.genderID || a.className != b.className ||
			!eq(a.race, b.race) || !eq(a.gender, b.gender) || !eq(a.class, b.class) {
			t.Errorf("%s changed when the migration ran again: %+v, was %+v", a.name, b, a)
		}
	}
	// The same warnings each time (the rows are still left), and a summary of nothing derived.
	warnings := func(list []string) (n int) {
		for _, s := range list {
			if strings.HasPrefix(s, "WARNING: migration 0003: ") {
				n++
			}
		}
		return n
	}
	again := notes.taking()
	if want := warnings(firstNotes); want == 0 || warnings(again) != 2*want {
		t.Errorf("%d warnings the first time, %d over two more runs", want, warnings(again))
	}
	if !strings.Contains(strings.Join(again, " | "), "race derived for 0, gender for 0, class for 0") {
		t.Errorf("a second run derived something: %v", again)
	}

	// A row the services wrote after the migration: its columns hold what they said, whatever the JSON or the
	// legacy columns say, and running the migration again leaves them alone.
	if _, err := pool.Exec(ctx, `INSERT INTO characters (id, account_id, name, race, gender, class_id, archetype_id, class_name, appearance)
		SELECT 'ffffffffffffffffffffffffffffffff', account_id, 'Service', 3, 1, 5, 17752, 'Rogue', '{"race":"Dunir","gender":"Male","class":"Bard"}'
		FROM characters LIMIT 1`); err != nil {
		t.Fatal(err)
	}
	// ... and one whose derived value was corrected by hand
	if _, err := pool.Exec(ctx, `UPDATE characters SET race = 5 WHERE name = 'DunirTank'`); err != nil {
		t.Fatal(err)
	}
	run()
	after := rows(t, pool, true)
	svc := after["ffffffffffffffffffffffffffffffff"]
	if *svc.race != 3 || *svc.gender != 1 || *svc.class != 5 || svc.raceID != 0 || svc.genderID != 0 {
		t.Errorf("a row the services wrote: race %s gender %s class %s legacy %d %d", show(svc.race), show(svc.gender), show(svc.class), svc.raceID, svc.genderID)
	}
	var tank *int
	for _, s := range after {
		if s.name == "DunirTank" {
			tank = s.race
		}
	}
	if tank == nil || *tank != 5 {
		t.Errorf("a value set since the migration was overwritten: race %s", show(tank))
	}
}

// The new columns refuse values outside the enums, and the legacy ones may be
// left out of an insert.
func TestMigration0003Constraints(t *testing.T) {
	ctx := context.Background()
	pool := dbtest.Pool(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	var account int64
	if err := pool.QueryRow(ctx, `INSERT INTO accounts (username, password_hash) VALUES ('Owner', 'x') RETURNING id`).Scan(&account); err != nil {
		t.Fatal(err)
	}
	insert := func(n int, race, gender, class any) error {
		_, err := pool.Exec(ctx, `INSERT INTO characters (id, account_id, name, race, gender, class_id, archetype_id, class_name)
			VALUES ($1, $2, $3, $4, $5, $6, 17747, 'Bard')`, fmt.Sprintf("%032x", n), account, fmt.Sprintf("Char%d", n), race, gender, class)
		return err
	}
	for i, tc := range []struct {
		name                string
		race, gender, class any
		want                string // constraint, "" for accepted
	}{
		{"a Kaelar male Bard", 0, 0, 2, ""},
		{"Tulnar is in the enum", 8, 1, 8, ""},
		{"not derived", nil, nil, nil, ""},
		{"race 9", 9, 0, 2, "characters_race_enum"},
		{"race -1", -1, 0, 2, "characters_race_enum"},
		{"gender 2", 0, 2, 2, "characters_gender_enum"},
		{"class 0", 0, 0, 0, "characters_class_enum"},
		{"class 9", 0, 0, 9, "characters_class_enum"},
	} {
		err := insert(i+1, tc.race, tc.gender, tc.class)
		var pgErr *pgconn.PgError
		switch {
		case tc.want == "" && err != nil:
			t.Errorf("%s: %v", tc.name, err)
		case tc.want != "" && (err == nil || !asPg(err, &pgErr) || pgErr.ConstraintName != tc.want):
			t.Errorf("%s: %v, want the constraint %s", tc.name, err, tc.want)
		}
	}
}

// [C-Y-B x C O A]

// A table that stays locked fails the start with a lock timeout (SQLSTATE 55P03) instead of waiting for ever, and
// leaves nothing applied: not the columns, not the version. The retry, once the lock is gone, works. The migration's
// own 10 s is shortened to 200 ms in the copy this test runs.
func TestMigration0003FailsOnAHeldLockAndTheRetryWorks(t *testing.T) {
	ctx := context.Background()
	pool, _, legacy := legacySetup(t)
	original, err := migrations.ReadFile(migration0003)
	if err != nil {
		t.Fatal(err)
	}
	const timeout = "SET LOCAL lock_timeout = '10s';"
	if !bytes.Contains(original, []byte(timeout)) {
		t.Fatalf("the migration has no %q to shorten for this test", timeout)
	}
	files := upTo(t, 4)
	files["0003_character_race_class.sql"] = &fstest.MapFile{Data: bytes.Replace(original, []byte(timeout), []byte("SET LOCAL lock_timeout = '200ms';"), 1)}

	// another transaction has a row of characters locked for update, as a running service's save would
	holder, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer holder.Rollback(ctx)
	if _, err := holder.Exec(ctx, `UPDATE characters SET level = level`); err != nil {
		t.Fatal(err)
	}
	start := time.Now()
	err = migrate(ctx, pool, files)
	var pgErr *pgconn.PgError
	if err == nil || !asPg(err, &pgErr) || pgErr.Code != "55P03" {
		t.Fatalf("migrate with the table locked: %v, want SQLSTATE 55P03 (lock_not_available)", err)
	}
	if waited := time.Since(start); waited < 150*time.Millisecond || waited > 5*time.Second {
		t.Errorf("the migration waited %s for the lock, want about 200 ms", waited)
	}
	var recorded, columns int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 3`).Scan(&recorded); err != nil {
		t.Fatal(err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM information_schema.columns
		WHERE table_schema = current_schema() AND table_name = 'characters' AND column_name IN ('race', 'gender', 'class_id')`).Scan(&columns); err != nil {
		t.Fatal(err)
	}
	if recorded != 0 || columns != 0 {
		t.Errorf("after the failure version 3 is recorded %d times and %d of the new columns exist, want none", recorded, columns)
	}

	if err := holder.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	if err := migrate(ctx, pool, files); err != nil {
		t.Fatalf("the retry after the lock is gone: %v", err)
	}
	after := rows(t, pool, true)
	if len(after) != len(legacy) {
		t.Fatalf("%d rows after the retry, %d before", len(after), len(legacy))
	}
	if c := after["bbec5c86eb101fe5f38c87f0fa22e7d0"]; c.race == nil || *c.race != 0 || *c.gender != 0 || *c.class != 2 {
		t.Errorf("cyb after the retry: %+v", c)
	}
}

// -- C O A & C | Y | B --

// The rollback in services/README.md ("Rolling back migration 0003"), run as written: a row a B1 service wrote
// (race_id and gender_id 0) gets the class and the 1/2 gender the pre-B1 services read, the rows that were migrated
// keep what they had, the first schema is back, and the migration can be applied again.
func TestMigration0003RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0003"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	rollback := block[:strings.Index(block, "\n```")]

	pool, _, legacy := legacySetup(t)
	before := rows(t, pool, false)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	// a character a B1 service made: its columns set, the legacy ones left at their defaults
	const made = "ffffffffffffffffffffffffffffffff"
	if _, err := pool.Exec(ctx, `INSERT INTO characters (id, account_id, name, race, gender, class_id, archetype_id, class_name, appearance)
		SELECT $1, account_id, 'Service', 6, 1, 7, 17754, 'Tank', '{"race":"Dunir","gender":"Female","class":"Tank"}'
		FROM characters LIMIT 1`, made); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("the README's rollback: %v", err)
	}

	after := map[string]snapshot{}
	r, err := pool.Query(ctx, `SELECT id, name, race_id, gender_id, archetype_id, class_name, appearance::text, level FROM characters`)
	if err != nil {
		t.Fatal(err)
	}
	for r.Next() {
		var id string
		var s snapshot
		if err := r.Scan(&id, &s.name, &s.raceID, &s.genderID, &s.archetypeID, &s.className, &s.appearance, &s.level); err != nil {
			t.Fatal(err)
		}
		after[id] = s
	}
	r.Close()
	if len(after) != len(legacy)+1 {
		t.Fatalf("%d rows after the rollback, want %d", len(after), len(legacy)+1)
	}
	for _, row := range legacy {
		if got, want := after[row.id], before[row.id]; got != want {
			t.Errorf("%s after the rollback: %+v, was %+v", row.name, got, want)
		}
	}
	if s := after[made]; s.raceID != 7 || s.genderID != 2 {
		t.Errorf("the B1 row after the rollback: race_id %d gender_id %d, want its class 7 and gender 2 (Female)", s.raceID, s.genderID)
	}
	// the first schema again: no new columns, no defaults, no record of the version, race_id and gender_id required
	var columns, recorded int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM information_schema.columns WHERE table_schema = current_schema()
		AND table_name = 'characters' AND (column_name IN ('race', 'gender', 'class_id')
		OR (column_name IN ('race_id', 'gender_id') AND column_default IS NOT NULL))`).Scan(&columns); err != nil {
		t.Fatal(err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 3`).Scan(&recorded); err != nil {
		t.Fatal(err)
	}
	if columns != 0 || recorded != 0 {
		t.Errorf("after the rollback %d of the new columns or defaults remain, version 3 recorded %d times", columns, recorded)
	}
	_, err = pool.Exec(ctx, `INSERT INTO characters (id, account_id, name, archetype_id, class_name)
		SELECT 'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee', account_id, 'NoLegacy', 17747, 'Bard' FROM characters LIMIT 1`)
	var pgErr *pgconn.PgError
	if err == nil || !asPg(err, &pgErr) || pgErr.Code != "23502" {
		t.Errorf("an insert without race_id: %v, want a not-null violation as in the first schema", err)
	}
	// ... and the migration applies again
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("the migration after the rollback: %v", err)
	}
	if c := rows(t, pool, true)[made]; c.race == nil || c.class == nil || *c.class != 7 {
		t.Errorf("the B1 row after the migration is applied again: %+v", c)
	}
}

func asPg(err error, target **pgconn.PgError) bool {
	for err != nil {
		if e, ok := err.(*pgconn.PgError); ok {
			*target = e
			return true
		}
		u, ok := err.(interface{ Unwrap() error })
		if !ok {
			return false
		}
		err = u.Unwrap()
	}
	return false
}

// [C:Y:B + C.O.A]

// An empty table is migrated too (a fresh database), and a table with rows whose
// columns exist already (the migration's own ADD COLUMN IF NOT EXISTS) is not an error.
func TestMigration0003OnAnEmptyTableAndWithTheColumnsPresent(t *testing.T) {
	ctx := context.Background()
	pool := dbtest.Pool(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	sql, err := migrations.ReadFile(migration0003)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, string(sql)); err != nil {
		t.Errorf("the migration run on a migrated empty table: %v", err)
	}
}

// db.Open sends PostgreSQL's warnings and a migration's notices to the log.
func TestOpenLogsWarningsAndMigrationNotices(t *testing.T) {
	url := os.Getenv("AOC_TEST_DATABASE_URL")
	if url == "" {
		t.Skip("AOC_TEST_DATABASE_URL not set (scripts/services.ps1 test sets it)")
	}
	var buf bytes.Buffer
	saved := slog.Default()
	slog.SetDefault(slog.New(slog.NewTextHandler(&buf, &slog.HandlerOptions{Level: slog.LevelDebug})))
	defer slog.SetDefault(saved)

	ctx := context.Background()
	pool, err := Open(ctx, url)
	if err != nil {
		t.Fatal(err)
	}
	defer pool.Close()
	if _, err := pool.Exec(ctx, `DO $$ BEGIN
		RAISE NOTICE 'migration 0003: 5 characters';
		RAISE WARNING 'migration 0003: character x was left without a race';
		RAISE NOTICE 'something else';
	END $$`); err != nil {
		t.Fatal(err)
	}
	out := buf.String()
	for _, want := range []string{
		`level=INFO msg="postgres: migration 0003: 5 characters"`,
		`level=WARN msg="postgres: migration 0003: character x was left without a race"`,
		`level=DEBUG msg="postgres: something else"`,
	} {
		if !strings.Contains(out, want) {
			t.Errorf("log does not contain %s:\n%s", want, out)
		}
	}
}
