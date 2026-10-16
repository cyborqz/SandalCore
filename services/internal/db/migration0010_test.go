// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0010 (Gathering stage 2, "Gathering data"): the professions of a character. It ADDS ONE TABLE, character_professions, and backfills it: 22 rows (Novice,
// level 1, XP 0) for every character that already exists, since every character holds Novice certification in all 22 CharacterProfessions (the user's rule of
// 2026-10-09). Nothing outside the new table changes. It is tested for what it must not do first (every character reads back as it was, no other table or column
// changes), then for what it makes (the 22 rows of each character, the constraints, the cascade), a second run changes nothing, the 22 guids are content.db's, and the rollback
// is the README's block, run as written.

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5/pgxpool"

	"aocrop/services/internal/content"
)

// before10 is the schema as it is after 0009 (the play database of the Nodes 1 test), with the real-shaped characters.
func before10(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool, _, _ := legacySetup(t)
	if err := migrate(context.Background(), pool, upTo(t, 10)); err != nil {
		t.Fatalf("migrate to 0009: %v", err)
	}
	return pool
}

// (CoA : C.Y.B)

// schemaOf lists every column of every table but character_professions (and the table migration 0011 adds, which Migrate applies after it), with its type, nullability and default, and the constraints: what 0010 must not change.
func schemaOf(t *testing.T, pool *pgxpool.Pool) []string {
	t.Helper()
	ctx := context.Background()
	var out []string
	cols, err := pool.Query(ctx, `SELECT table_name || '.' || column_name || ' ' || data_type || ' ' || is_nullable || ' ' || coalesce(column_default, '-') FROM information_schema.columns
		WHERE table_schema = current_schema() AND table_name NOT IN ('character_professions', 'character_resource_items') ORDER BY 1`)
	if err != nil {
		t.Fatal(err)
	}
	for cols.Next() {
		var s string
		if err := cols.Scan(&s); err != nil {
			t.Fatal(err)
		}
		out = append(out, s)
	}
	cols.Close()
	cons, err := pool.Query(ctx, `SELECT conrelid::regclass::text || ' ' || conname || ' ' || pg_get_constraintdef(oid) FROM pg_constraint
		WHERE connamespace = current_schema()::regnamespace AND conrelid::regclass::text NOT IN ('character_professions', 'character_resource_items') ORDER BY 1`)
	if err != nil {
		t.Fatal(err)
	}
	for cons.Next() {
		var s string
		if err := cons.Scan(&s); err != nil {
			t.Fatal(err)
		}
		out = append(out, s)
	}
	cons.Close()
	tables, err := pool.Query(ctx, `SELECT 'table ' || table_name FROM information_schema.tables WHERE table_schema = current_schema() AND table_name NOT IN ('character_professions', 'character_resource_items') ORDER BY 1`)
	if err != nil {
		t.Fatal(err)
	}
	for tables.Next() {
		var s string
		if err := tables.Scan(&s); err != nil {
			t.Fatal(err)
		}
		out = append(out, s)
	}
	tables.Close()
	return out
}

func professionGuidsInSQL(t *testing.T, pool *pgxpool.Pool) []int64 {
	t.Helper()
	r, err := pool.Query(context.Background(), `SELECT DISTINCT profession_guid FROM character_professions ORDER BY 1`)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	var out []int64
	for r.Next() {
		var g int64
		if err := r.Scan(&g); err != nil {
			t.Fatal(err)
		}
		out = append(out, g)
	}
	return out
}

func TestMigration0010AddsOneTableAndTouchesNothingElse(t *testing.T) {
	ctx := context.Background()
	pool := before10(t)
	chars := rows(t, pool, true)
	if len(chars) == 0 {
		t.Fatal("the play database has no characters")
	}
	var has bool
	if err := pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = 'character_professions')`).Scan(&has); err != nil || has {
		t.Fatalf("character_professions exists before 0010 (%v, %v)", has, err)
	}
	schemaBefore := schemaOf(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	// nothing else changed: the same tables, columns, defaults and constraints, and every character as it was
	if after := schemaOf(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Errorf("0010 changed more than character_professions:\nbefore %d entries, after %d", len(schemaBefore), len(after))
	}
	after := rows(t, pool, true)
	if len(after) != len(chars) {
		t.Fatalf("%d characters after, %d before", len(after), len(chars))
	}
	for id, b := range chars {
		a := after[id]
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil // pointers: compared by value
		if a != b {
			t.Errorf("%s changed by 0010", b.name)
		}
	}

	// the backfill: 22 rows for each character that existed, all Novice, level 1, XP 0
	var total, rowsOfCybs, distinctChars int
	if err := pool.QueryRow(ctx, `SELECT count(*), count(*) FILTER (WHERE character_id = $1), count(DISTINCT character_id) FROM character_professions`, cybID).Scan(&total, &rowsOfCybs, &distinctChars); err != nil {
		t.Fatal(err)
	}
	if total != 22*len(chars) || rowsOfCybs != 22 || distinctChars != len(chars) {
		t.Errorf("character_professions has %d rows (%d for one character, %d characters), want %d, 22 and %d", total, rowsOfCybs, distinctChars, 22*len(chars), len(chars))
	}
	var notNovice int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_professions WHERE certification_tier <> 0 OR level <> 1 OR xp <> 0`).Scan(&notNovice); err != nil || notNovice != 0 {
		t.Errorf("%d backfilled rows are not Novice, level 1, XP 0 (%v)", notNovice, err)
	}
	if guids := professionGuidsInSQL(t, pool); len(guids) != 22 {
		t.Errorf("%d distinct professions in the backfill, want 22", len(guids))
	}

	// the constraints
	for _, bad := range []string{
		`INSERT INTO character_professions (character_id, profession_guid) VALUES ('` + cybID + `', 108752769996425)`,           // the primary key
		`INSERT INTO character_professions (character_id, profession_guid) VALUES ('no such character', 108752769996425)`,       // the foreign key
		`INSERT INTO character_professions (character_id, profession_guid, certification_tier) VALUES ('` + cybID + `', 1, 5)`,  // tier above GrandMaster
		`INSERT INTO character_professions (character_id, profession_guid, certification_tier) VALUES ('` + cybID + `', 1, -1)`, // tier below Novice
		`INSERT INTO character_professions (character_id, profession_guid, level) VALUES ('` + cybID + `', 1, 0)`,               // level 0
		`INSERT INTO character_professions (character_id, profession_guid, level) VALUES ('` + cybID + `', 1, 51)`,              // above the cap
		`INSERT INTO character_professions (character_id, profession_guid, xp) VALUES ('` + cybID + `', 1, -1)`,                 // negative XP
		`INSERT INTO character_professions (character_id, profession_guid) VALUES ('` + cybID + `', 0)`,                         // no profession
		`INSERT INTO character_professions (profession_guid) VALUES (1)`,                                                        // no character
		`UPDATE character_professions SET level = 51 WHERE character_id = '` + cybID + `'`,                                      // the checks hold for updates too
	} {
		if _, err := pool.Exec(ctx, bad); err == nil {
			t.Errorf("accepted: %s", bad)
		}
	}
	// a row takes the values a profession can have, and goes with its character
	if _, err := pool.Exec(ctx, `UPDATE character_professions SET certification_tier = 2, level = 25, xp = 250000 WHERE character_id = $1 AND profession_guid = 108752769996425`, cybID); err != nil {
		t.Fatalf("a Journeyman level 25 Miner: %v", err)
	}
	var tier, level int
	var xp int64
	if err := pool.QueryRow(ctx, `SELECT certification_tier, level, xp FROM character_professions WHERE character_id = $1 AND profession_guid = 108752769996425`, cybID).Scan(&tier, &level, &xp); err != nil ||
		tier != 2 || level != 25 || xp != 250000 {
		t.Errorf("the Miner reads back as %d %d %d (%v)", tier, level, xp, err)
	}

	// a second run changes nothing (the Miner keeps its level)
	if err := Migrate(ctx, pool); err != nil {
		t.Errorf("second migrate: %v", err)
	}
	if err := pool.QueryRow(ctx, `SELECT certification_tier, level, xp FROM character_professions WHERE character_id = $1 AND profession_guid = 108752769996425`, cybID).Scan(&tier, &level, &xp); err != nil ||
		tier != 2 || level != 25 || xp != 250000 {
		t.Errorf("after the second migrate the Miner reads %d %d %d (%v)", tier, level, xp, err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_professions`).Scan(&total); err != nil || total != 22*len(chars) {
		t.Errorf("after the second migrate %d rows (%v), want %d", total, err, 22*len(chars))
	}
	// deleting a character takes its professions along
	if _, err := pool.Exec(ctx, `DELETE FROM characters WHERE id = $1`, cybID); err != nil {
		t.Fatal(err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_professions WHERE character_id = $1`, cybID).Scan(&total); err != nil || total != 0 {
		t.Errorf("%d profession rows outlive their character (%v)", total, err)
	}
}

// ~ C.Y.B :: CoA ~

// The 22 guids of the SQL are the 22 CharacterProfessions of content.db (the migration cannot read the file: a test holds the two together).
func TestMigration0010ProfessionGuidsAreContentDBs(t *testing.T) {
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
	if err != nil || len(profs) != 22 {
		t.Fatalf("Professions = %d, %v", len(profs), err)
	}
	var want []int64
	for _, p := range profs {
		want = append(want, p.Guid)
	}
	sort.Slice(want, func(i, j int) bool { return want[i] < want[j] })

	pool := before10(t)
	if err := Migrate(context.Background(), pool); err != nil {
		t.Fatal(err)
	}
	got := professionGuidsInSQL(t, pool)
	if fmt.Sprint(got) != fmt.Sprint(want) {
		t.Errorf("the migration's professions are %v, content.db's are %v", got, want)
	}
}

// The rollback in services/README.md ("Rolling back migration 0010"), run as written: the table and the record of the version go, the characters are as they
// were, and the migration applies again (and backfills again).
func TestMigration0010RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0010"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	rollback := block[:strings.Index(block, "\n```")]

	pool := before10(t)
	chars := rows(t, pool, true)
	schemaBefore := schemaOf(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `UPDATE character_professions SET level = 12 WHERE character_id = $1 AND profession_guid = 108752769996425`, cybID); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("the README's rollback: %v", err)
	}
	var has bool
	if err := pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = 'character_professions')`).Scan(&has); err != nil || has {
		t.Errorf("character_professions is still there after the rollback (%v, %v)", has, err)
	}
	var recorded int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 10`).Scan(&recorded); err != nil || recorded != 0 {
		t.Errorf("version 10 is still recorded after the rollback (%d, %v)", recorded, err)
	}
	if after := schemaOf(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Error("the rollback left the schema other than it was before 0010")
	}
	after := rows(t, pool, true)
	for id, b := range chars {
		a := after[id]
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil
		if a != b {
			t.Errorf("%s changed by the migration and its rollback", b.name)
		}
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate again after the rollback: %v", err)
	}
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_professions`).Scan(&n); err != nil || n != 22*len(chars) {
		t.Errorf("after migrating again %d rows (%v), want %d", n, err, 22*len(chars))
	}
}
