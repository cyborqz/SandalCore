// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0021 (social, step 6: the ignore list). It ADDS ONE TABLE, character_ignored_names, and touches nothing else (every migration is add-only). It is tested for what it must not do first
// (every character reads back as it was, no other table, column, constraint or index changes), then for what it makes (the checks, the keys, the cascade that removes a deleted character's
// names), a second run changes nothing, and the rollback is the README's block, run as written. The last tests are about the number: a database that ran 0021 before 0020 (node
// citizenship, numbered first and landed later) still takes 0020, and the runner refuses a second file with a version it already has.

import (
	"context"
	"os"
	"slices"
	"strings"
	"testing"
	"testing/fstest"

	"github.com/jackc/pgx/v5/pgxpool"

	"aocrop/services/internal/dbtest"
)

const ignoredNamesTable = "character_ignored_names"

// before21 is the schema as it is before 0021 (after 0020), with the real-shaped characters.
func before21(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool, _, _ := legacySetup(t)
	if err := migrate(context.Background(), pool, upTo(t, 21)); err != nil {
		t.Fatalf("migrate to 0020: %v", err)
	}
	return pool
}

// schemaWithoutIgnoredNames lists every column, constraint, index and table but those of character_ignored_names: what 0021 must not change.
func schemaWithoutIgnoredNames(t *testing.T, pool *pgxpool.Pool) []string {
	t.Helper()
	var out []string
	for _, q := range []string{
		`SELECT table_name || '.' || column_name || ' ' || data_type || ' ' || is_nullable || ' ' || coalesce(column_default, '-') FROM information_schema.columns
			WHERE table_schema = current_schema() AND table_name NOT IN ('character_ignored_names', 'character_node_reputation') ORDER BY 1`,
		`SELECT conrelid::regclass::text || ' ' || conname || ' ' || pg_get_constraintdef(oid) FROM pg_constraint
			WHERE connamespace = current_schema()::regnamespace AND conrelid::regclass::text NOT IN ('character_ignored_names', 'character_node_reputation') ORDER BY 1`,
		`SELECT 'index ' || tablename || ' ' || indexdef FROM pg_indexes WHERE schemaname = current_schema() AND tablename NOT IN ('character_ignored_names', 'character_node_reputation') ORDER BY 1`,
		`SELECT 'table ' || table_name FROM information_schema.tables WHERE table_schema = current_schema() AND table_name NOT IN ('character_ignored_names', 'character_node_reputation') ORDER BY 1`,
	} {
		r, err := pool.Query(context.Background(), q)
		if err != nil {
			t.Fatal(err)
		}
		for r.Next() {
			var s string
			if err := r.Scan(&s); err != nil {
				t.Fatal(err)
			}
			out = append(out, s)
		}
		r.Close()
	}
	return out
}

func TestMigration0021AddsTheIgnoredNamesTableAndTouchesNothingElse(t *testing.T) {
	ctx := context.Background()
	pool := before21(t)
	chars := rows(t, pool, true)
	if len(chars) == 0 {
		t.Fatal("the play database has no characters")
	}
	if tableExists(t, pool, ignoredNamesTable) {
		t.Fatalf("%s exists before 0021", ignoredNamesTable)
	}
	schemaBefore := schemaWithoutIgnoredNames(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	if !tableExists(t, pool, ignoredNamesTable) {
		t.Fatalf("0021 did not make %s", ignoredNamesTable)
	}
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_ignored_names`).Scan(&n); err != nil || n != 0 {
		t.Errorf("%s has %d rows after the migration (%v), want none: nothing is backfilled", ignoredNamesTable, n, err)
	}
	if after := schemaWithoutIgnoredNames(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Errorf("0021 changed more than its one table:\nbefore %d entries, after %d", len(schemaBefore), len(after))
	}
	after := rows(t, pool, true)
	if len(after) != len(chars) {
		t.Fatalf("%d characters after, %d before", len(after), len(chars))
	}
	for id, b := range chars {
		a := after[id]
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil // pointers: compared by value
		if a != b {
			t.Errorf("%s changed by 0021", b.name)
		}
	}
	// the version is recorded once, after 0020 (node citizenship, which before21 applied)
	versions := appliedVersions(t, pool)
	if !slices.Contains(versions, 21) || !slices.Contains(versions, 20) {
		t.Errorf("applied versions = %v, want 20 and 21 in it", versions)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Errorf("second migrate: %v", err)
	}
	if again := appliedVersions(t, pool); !slices.Equal(again, versions) {
		t.Errorf("the second migrate changed the applied versions: %v then %v", versions, again)
	}
}

func TestIgnoredNamesConstraints(t *testing.T) {
	ctx := context.Background()
	pool := before21(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	put := func(character, kind, name string, position int) error {
		_, err := pool.Exec(ctx, `INSERT INTO character_ignored_names (character_id, kind, name, position) VALUES ($1, $2, $3, $4)`, character, kind, name, position)
		return err
	}
	// what a list is: players and guilds, a name with a space inside, a letter outside ASCII, the longest name, and the same name in the two lists
	for _, tc := range []struct {
		name, kind, value string
		position          int
	}{
		{"a player", "character", "Brannoc", 0},
		{"a second player", "character", "Mirelle", 1},
		{"the same name as a guild", "guild", "Brannoc", 0},
		{"a guild with a space inside", "guild", "Sandal Makers", 1},
		{"a letter outside ASCII", "character", "Zoë", 2},
		{"the longest name", "character", strings.Repeat("n", 64), 3},
	} {
		if err := put(cybID, tc.kind, tc.value, tc.position); err != nil {
			t.Errorf("%s: %v", tc.name, err)
		}
	}
	// a second character keeps a list of its own, with the same names and places
	if err := put("00000000000000000000000000000001", "character", "Brannoc", 0); err != nil {
		t.Errorf("another character ignoring the same name: %v", err)
	}
	for _, tc := range []struct {
		name       string
		err        error
		code       string
		constraint string
	}{
		{"an empty name", put(cybID, "character", "", 10), "23514", "character_ignored_names_name"},
		{"a space in front", put(cybID, "character", " Brannoc2", 11), "23514", "character_ignored_names_name"},
		{"a space behind", put(cybID, "character", "Brannoc3 ", 12), "23514", "character_ignored_names_name"},
		{"a name of 65 characters", put(cybID, "character", strings.Repeat("n", 65), 13), "23514", "character_ignored_names_name"},
		{"a kind that is none", put(cybID, "player", "Brannoc4", 14), "23514", "character_ignored_names_kind"},
		{"a negative place", put(cybID, "character", "Brannoc5", -1), "23514", "character_ignored_names_position"},
		{"a name twice in one list", put(cybID, "character", "Brannoc", 15), "23505", "character_ignored_names_pkey"},
		{"a place twice in one list", put(cybID, "character", "Brannoc6", 0), "23505", "character_ignored_names_order"},
		{"a character that does not exist", put("ffffffffffffffffffffffffffffffff", "character", "Brannoc7", 0), "23503", "character_ignored_names_character_id_fkey"},
	} {
		expectConstraint(t, tc.name, tc.err, tc.code, tc.constraint)
	}
	var count int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_ignored_names WHERE character_id = $1`, cybID).Scan(&count); err != nil || count != 6 {
		t.Errorf("cyb keeps %d names (%v), want the 6 that were accepted", count, err)
	}
}

// The names go with their character: deleting it removes its rows and no other character's.
func TestIgnoredNamesAreRemovedWithTheirCharacter(t *testing.T) {
	ctx := context.Background()
	pool := before21(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	const other = "00000000000000000000000000000001"
	for _, id := range []string{cybID, other} {
		if _, err := pool.Exec(ctx, `INSERT INTO character_ignored_names (character_id, kind, name, position) VALUES ($1, 'character', 'Brannoc', 0), ($1, 'guild', 'Sandal Makers', 0)`, id); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := pool.Exec(ctx, `DELETE FROM characters WHERE id = $1`, cybID); err != nil {
		t.Fatal(err)
	}
	var mine, theirs int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_ignored_names WHERE character_id = $1`, cybID).Scan(&mine); err != nil || mine != 0 {
		t.Errorf("a deleted character keeps %d ignored names (%v)", mine, err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_ignored_names WHERE character_id = $1`, other).Scan(&theirs); err != nil || theirs != 2 {
		t.Errorf("another character keeps %d ignored names (%v), want its 2", theirs, err)
	}
}

// The rollback in services/README.md ("Rolling back migration 0021"), run as written: the table, its names and the record of the version go, nothing else changes, and the migration applies again.
func TestMigration0021RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0021"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	rollback := block[:strings.Index(block, "\n```")]

	pool := before21(t)
	schemaBefore := schemaWithoutIgnoredNames(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_ignored_names (character_id, kind, name, position) VALUES ($1, 'character', 'Brannoc', 0)`, cybID); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("the README's rollback: %v", err)
	}
	if tableExists(t, pool, ignoredNamesTable) {
		t.Errorf("%s is still there after the rollback", ignoredNamesTable)
	}
	var recorded int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 21`).Scan(&recorded); err != nil || recorded != 0 {
		t.Errorf("version 21 is still recorded after the rollback (%d, %v)", recorded, err)
	}
	if after := schemaWithoutIgnoredNames(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Error("the rollback left the schema other than it was before 0021")
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate again after the rollback: %v", err)
	}
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_ignored_names`).Scan(&n); err != nil || n != 0 {
		t.Errorf("after migrating again the table holds %d rows (%v), want none", n, err)
	}
}

// The number. 0021 left 0020 free for node citizenship (0020_account_citizenship.sql), numbered first and landed later. The runner has to take that: a database that already holds 0021
// without 0020 applies 0020 when it comes, a new one applies both, and the one thing it refuses is two files with one version, which stops every service that migrates when it starts.
func TestMigration0020CanLandAfter0021(t *testing.T) {
	ctx := context.Background()
	if !slices.Contains(embeddedVersions(t), 20) || !slices.Contains(embeddedVersions(t), 21) {
		t.Fatal("migrations 0020 and 0021 are not both embedded")
	}
	pool := dbtest.Pool(t)
	with20 := upTo(t, 22) // the embedded migrations 1 to 21
	without20 := fstest.MapFS{}
	for name, f := range with20 {
		if !strings.HasPrefix(name, "0020_") {
			without20[name] = f
		}
	}
	if len(without20) != len(with20)-1 {
		t.Fatalf("%d migrations without 0020 of %d", len(without20), len(with20))
	}
	if err := migrate(ctx, pool, without20); err != nil {
		t.Fatalf("migrate with the gap at 0020: %v", err)
	}
	if got := appliedVersions(t, pool); slices.Contains(got, 20) || !slices.Contains(got, 21) {
		t.Fatalf("applied versions = %v, want 21 and no 20", got)
	}
	if err := migrate(ctx, pool, with20); err != nil {
		t.Fatalf("migrate once 0020 has landed: %v", err)
	}
	if got := appliedVersions(t, pool); !slices.Contains(got, 20) || !slices.Contains(got, 21) {
		t.Errorf("applied versions = %v, want 20 and 21", got)
	}
	if !tableExists(t, pool, "account_citizenship") || !tableExists(t, pool, ignoredNamesTable) {
		t.Errorf("after 0020 landed: account_citizenship there = %v, %s there = %v", tableExists(t, pool, "account_citizenship"), ignoredNamesTable, tableExists(t, pool, ignoredNamesTable))
	}
	// the other order, a new database: 0020 first, then 0021, both there
	fresh := dbtest.Pool(t)
	if err := migrate(ctx, fresh, with20); err != nil {
		t.Fatalf("migrate a new database with both: %v", err)
	}
	if got := appliedVersions(t, fresh); !slices.Contains(got, 20) || !slices.Contains(got, 21) || !tableExists(t, fresh, ignoredNamesTable) {
		t.Errorf("a new database: applied versions = %v, %s there = %v", got, ignoredNamesTable, tableExists(t, fresh, ignoredNamesTable))
	}
	// a second file numbered 0021 is the collision: refused before anything is applied, and the error names both files
	clash := fstest.MapFS{}
	for name, f := range with20 {
		clash[name] = f
	}
	clash["0021_other_change.sql"] = &fstest.MapFile{Data: []byte("CREATE TABLE other_change (id INT);")}
	_, err := loadMigrations(clash)
	if err == nil || !strings.Contains(err.Error(), "0021_character_ignored_names.sql and 0021_other_change.sql") || !strings.Contains(err.Error(), "version 21") {
		t.Errorf("two files numbered 0021: %v, want the refusal naming both", err)
	}
}
