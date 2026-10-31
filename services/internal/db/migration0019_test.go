// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0019 (crafting quality: the artisan ratings a processing job was queued with). It ADDS THREE COLUMNS to character_processing_jobs, all with a default of 0, so it is tested for what it
// must not do first: the jobs a play database already holds are read back exactly as they were (with 0 in the new columns), nothing else in the schema changes, and a second run changes nothing. Then
// for what it makes (the column types, the checks), and the rollback is the README's block, run as written. The 0013 table is not edited: the play database has it already.

import (
	"context"
	"fmt"
	"math"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5/pgxpool"
)

var ratingColumns = []string{"rating_xp", "rating_quantity", "rating_rarity"}

// before19 is the schema as it is after 0018 (the play database as it stands), with the real-shaped characters and two jobs of cyb's.
func before19(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool, _, _ := legacySetup(t)
	if err := migrate(context.Background(), pool, upTo(t, 19)); err != nil {
		t.Fatalf("migrate to 0018: %v", err)
	}
	for i := 0; i < 2; i++ {
		if _, err := pool.Exec(context.Background(), `INSERT INTO character_processing_jobs (character_id, job_guid, ordinal, station_key, recipe_guid, quantity, queued_at, started_at, duration_ms, client_txn, resources)
			VALUES ($1, $2, $3, '0123456789abcdef', 6064632000000000001, 250, to_timestamp(1790000000 + $4::int), to_timestamp(1790000000 + $4::int), 300000, $5,
			'[{"record":109162192590532,"rarities":[{"rarity":1,"count":250,"warehouse":false}]}]'::jsonb)`,
			cybID, fmt.Sprintf("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa%d", i), i, i*10, fmt.Sprintf("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb%d", i)); err != nil {
			t.Fatalf("a job of the play database: %v", err)
		}
	}
	return pool
}

func hasRatingColumn(t *testing.T, pool *pgxpool.Pool, col string) bool {
	t.Helper()
	var has bool
	if err := pool.QueryRow(context.Background(), `SELECT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'character_processing_jobs' AND column_name = $1)`, col).Scan(&has); err != nil {
		t.Fatal(err)
	}
	return has
}

// jobSnapshot is every column of the jobs that 0019 does not add, as text, in the order of the table.
func jobSnapshot(t *testing.T, pool *pgxpool.Pool) []string {
	t.Helper()
	r, err := pool.Query(context.Background(), `SELECT character_id || '|' || job_guid || '|' || ordinal || '|' || station_key || '|' || recipe_guid || '|' || sub_recipe_guid || '|' || quantity || '|' ||
		extract(epoch FROM queued_at)::text || '|' || extract(epoch FROM started_at)::text || '|' || duration_ms || '|' || client_txn || '|' || resources::text
		FROM character_processing_jobs ORDER BY character_id, ordinal`)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	var out []string
	for r.Next() {
		var s string
		if err := r.Scan(&s); err != nil {
			t.Fatal(err)
		}
		out = append(out, s)
	}
	return out
}

// schemaWithoutRatings lists every column, constraint and table but the three columns and the three checks 0019 adds: what 0019 must not change.
func schemaWithoutRatings(t *testing.T, pool *pgxpool.Pool) []string {
	t.Helper()
	var out []string
	for _, q := range []string{
		`SELECT table_name || '.' || column_name || ' ' || data_type || ' ' || is_nullable || ' ' || coalesce(column_default, '-') FROM information_schema.columns
			WHERE table_schema = current_schema() AND NOT (table_name = 'character_processing_jobs' AND column_name IN ('rating_xp', 'rating_quantity', 'rating_rarity')) ORDER BY 1`,
		`SELECT conrelid::regclass::text || ' ' || conname || ' ' || pg_get_constraintdef(oid) FROM pg_constraint
			WHERE connamespace = current_schema()::regnamespace AND conname NOT IN ('character_processing_jobs_rating_xp', 'character_processing_jobs_rating_quantity', 'character_processing_jobs_rating_rarity') ORDER BY 1`,
		`SELECT 'table ' || table_name FROM information_schema.tables WHERE table_schema = current_schema() ORDER BY 1`,
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

func TestMigration0019AddsRatingColumnsAndTouchesNothingElse(t *testing.T) {
	ctx := context.Background()
	pool := before19(t)
	chars := rows(t, pool, true)
	jobs := jobSnapshot(t, pool)
	if len(jobs) != 2 {
		t.Fatalf("the play database's jobs: %d", len(jobs))
	}
	for _, col := range ratingColumns {
		if hasRatingColumn(t, pool, col) {
			t.Fatalf("%s exists before 0019", col)
		}
	}
	schemaBefore := schemaWithoutRatings(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	// every job reads as it did, with no ratings: 0, 0, 0
	if got := jobSnapshot(t, pool); strings.Join(got, "\n") != strings.Join(jobs, "\n") {
		t.Errorf("the jobs changed by 0019:\n%v\nwas\n%v", got, jobs)
	}
	var none int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_processing_jobs WHERE rating_xp = 0 AND rating_quantity = 0 AND rating_rarity = 0`).Scan(&none); err != nil || none != 2 {
		t.Errorf("%d of the 2 jobs keep no ratings after the migration (%v), want both", none, err)
	}
	if after := schemaWithoutRatings(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Errorf("0019 changed more than its three columns:\nbefore %d entries, after %d", len(schemaBefore), len(after))
	}
	after := rows(t, pool, true)
	if len(after) != len(chars) {
		t.Fatalf("%d characters after, %d before", len(after), len(chars))
	}
	for id, b := range chars {
		a := after[id]
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil // pointers: compared by value
		if a != b {
			t.Errorf("%s changed by 0019", b.name)
		}
	}
	// the column types and defaults: not-null double precision, default 0
	for _, col := range ratingColumns {
		var typ, nullable string
		var def *string
		if err := pool.QueryRow(ctx, `SELECT data_type, is_nullable, column_default FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'character_processing_jobs' AND column_name = $1`, col).Scan(&typ, &nullable, &def); err != nil {
			t.Fatal(err)
		}
		d := ""
		if def != nil {
			d = *def
		}
		if typ != "double precision" || nullable != "NO" || d != "0" {
			t.Errorf("%s is %s, nullable %s, default %q; want double precision, NO, 0", col, typ, nullable, d)
		}
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Errorf("second migrate: %v", err)
	}
	if got := jobSnapshot(t, pool); strings.Join(got, "\n") != strings.Join(jobs, "\n") {
		t.Error("the second migrate changed the jobs")
	}
}

func TestJobRatingConstraints(t *testing.T) {
	ctx := context.Background()
	pool := before19(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	n := 100
	put := func(xp, quantity, rarity float64) error {
		n++
		_, err := pool.Exec(ctx, `INSERT INTO character_processing_jobs (character_id, job_guid, ordinal, station_key, recipe_guid, quantity, queued_at, started_at, duration_ms, client_txn, rating_xp, rating_quantity, rating_rarity)
			VALUES ($1, $2, $3, '0123456789abcdef', 6064632000000000001, 1, now(), now(), 1000, $4, $5, $6, $7)`,
			cybID, fmt.Sprintf("%032x", n), n, fmt.Sprintf("%032x", 5000+n), xp, quantity, rarity)
		return err
	}
	// what a crafter's ratings are: a rating of 285.25, the bounds, none
	for name, args := range map[string][3]float64{
		"a crafter's ratings": {285.25, 337, 349.5},
		"none":                {0, 0, 0},
		"the bound":           {1e9, 1e9, 1e9},
		"a debug belt":        {99999, 0, 0},
	} {
		if err := put(args[0], args[1], args[2]); err != nil {
			t.Errorf("%s: %v", name, err)
		}
	}
	for _, tc := range []struct {
		name       string
		xp, q, r   float64
		constraint string
	}{
		{"a negative experience rating", -0.5, 0, 0, "character_processing_jobs_rating_xp"},
		{"an experience rating that is NaN", math.NaN(), 0, 0, "character_processing_jobs_rating_xp"},
		{"an infinite experience rating", math.Inf(1), 0, 0, "character_processing_jobs_rating_xp"},
		{"a negative quantity rating", 0, -1, 0, "character_processing_jobs_rating_quantity"},
		{"a quantity rating above 1e9", 0, 1.5e9, 0, "character_processing_jobs_rating_quantity"},
		{"a rarity rating that is NaN", 0, 0, math.NaN(), "character_processing_jobs_rating_rarity"},
		{"a negative rarity rating", 0, 0, -3, "character_processing_jobs_rating_rarity"},
	} {
		expectConstraint(t, tc.name, put(tc.xp, tc.q, tc.r), "23514", tc.constraint)
	}
}

// The rollback in services/README.md ("Rolling back migration 0019"), run as written: the three columns, their checks and the record of the version go, the jobs are as they were, and the
// migration applies again.
func TestMigration0019RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0019"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	rollback := block[:strings.Index(block, "\n```")]

	pool := before19(t)
	jobs := jobSnapshot(t, pool)
	schemaBefore := schemaWithoutRatings(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `UPDATE character_processing_jobs SET rating_xp = 285.25, rating_quantity = 337, rating_rarity = 349.5 WHERE ordinal = 1`); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("the README's rollback: %v", err)
	}
	for _, col := range ratingColumns {
		if hasRatingColumn(t, pool, col) {
			t.Errorf("%s is still there after the rollback", col)
		}
	}
	var recorded int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 19`).Scan(&recorded); err != nil || recorded != 0 {
		t.Errorf("version 19 is still recorded after the rollback (%d, %v)", recorded, err)
	}
	if got := jobSnapshot(t, pool); strings.Join(got, "\n") != strings.Join(jobs, "\n") {
		t.Errorf("the jobs after the rollback:\n%v\nwant\n%v", got, jobs)
	}
	if after := schemaWithoutRatings(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Error("the rollback left the schema other than it was before 0019")
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate again after the rollback: %v", err)
	}
	var none int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_processing_jobs WHERE rating_xp = 0 AND rating_quantity = 0 AND rating_rarity = 0`).Scan(&none); err != nil || none != 2 {
		t.Errorf("after migrating again %d of 2 jobs keep no ratings (%v)", none, err)
	}
}
