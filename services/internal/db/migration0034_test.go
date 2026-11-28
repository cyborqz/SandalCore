// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0034 (Marketplace storefront descriptions). It ADDS TWO TABLES, market_storefronts and node_market_headers, and touches nothing else (every migration is
// add-only). It is tested for what it must not do first (every character reads back as it was, no other table, column, constraint or index changes), then for what it makes
// (the exact columns and types, the keys and the fixed constraint names, the cascade), a second run changes nothing, and the rollback is the README's block, run as written,
// after which the migration applies again.

import (
	"context"
	"os"
	"slices"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5/pgxpool"

	"aocrop/services/internal/dbtest"
)

// storefrontTables are the two tables of migration 0034.
var storefrontTables = []string{"market_storefronts", "node_market_headers"}

// storefrontColumns is every column of the two tables, in the migration's order, with its type and nullability.
var storefrontColumns = map[string][]string{
	"market_storefronts":  {"node bigint NO", "character_id text NO", "description text NO", "updated_ms bigint NO"},
	"node_market_headers": {"node bigint NO", "header_raises bigint NO"},
}

// storefrontConstraints is every constraint name migration 0034 fixes.
var storefrontConstraints = []string{
	"market_storefronts_pkey", "market_storefronts_node", "market_storefronts_length", "market_storefronts_plain", "market_storefronts_updated",
	"market_storefronts_character_id_fkey", "node_market_headers_pkey", "node_market_headers_node", "node_market_headers_raises",
}

// before34 is the schema as it is at the migration before this one (0031, the sales; 0032 and 0033 are not in this tree), so 34 stops below this migration.
func before34(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool, _, _ := legacySetup(t)
	if err := migrate(context.Background(), pool, upTo(t, 34)); err != nil {
		t.Fatalf("migrate to 0031: %v", err)
	}
	return pool
}

// schemaWithoutStorefronts lists every column, constraint, index and table but the ones of 0034: what 0034 must not change.
func schemaWithoutStorefronts(t *testing.T, pool *pgxpool.Pool) []string {
	t.Helper()
	const skip = `('market_storefronts', 'node_market_headers')`
	var out []string
	for _, q := range []string{
		`SELECT table_name || '.' || column_name || ' ' || data_type || ' ' || is_nullable || ' ' || coalesce(column_default, '-') FROM information_schema.columns
			WHERE table_schema = current_schema() AND table_name NOT IN ` + skip + ` ORDER BY 1`,
		`SELECT conrelid::regclass::text || ' ' || conname || ' ' || pg_get_constraintdef(oid) FROM pg_constraint
			WHERE connamespace = current_schema()::regnamespace AND conrelid::regclass::text NOT IN ` + skip + ` ORDER BY 1`,
		`SELECT 'index ' || tablename || ' ' || indexdef FROM pg_indexes WHERE schemaname = current_schema() AND tablename NOT IN ` + skip + ` ORDER BY 1`,
		`SELECT 'table ' || table_name FROM information_schema.tables WHERE table_schema = current_schema() AND table_name NOT IN ` + skip + ` ORDER BY 1`,
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

func TestMigration0034AddsTwoTablesAndTouchesNothingElse(t *testing.T) {
	ctx := context.Background()
	pool := before34(t)
	chars := rows(t, pool, true)
	if len(chars) == 0 {
		t.Fatal("the play database has no characters")
	}
	for _, table := range storefrontTables {
		if tableExists(t, pool, table) {
			t.Fatalf("%s exists before 0034", table)
		}
	}
	schemaBefore := schemaWithoutStorefronts(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	for _, table := range storefrontTables {
		if !tableExists(t, pool, table) {
			t.Fatalf("0034 did not make %s", table)
		}
		var n int
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM `+table).Scan(&n); err != nil || n != 0 {
			t.Errorf("%s has %d rows after the migration (%v), want none: nothing is backfilled", table, n, err)
		}
	}
	if after := schemaWithoutStorefronts(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Errorf("0034 changed more than its tables:\nbefore %d entries, after %d", len(schemaBefore), len(after))
	}
	after := rows(t, pool, true)
	if len(after) != len(chars) {
		t.Fatalf("%d characters after, %d before", len(after), len(chars))
	}
	for id, b := range chars {
		a := after[id]
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil // pointers: compared by value
		if a != b {
			t.Errorf("%s changed by 0034", b.name)
		}
	}
	versions := appliedVersions(t, pool)
	if !slices.Contains(versions, 34) {
		t.Errorf("applied versions = %v, want 34 in it", versions)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Errorf("second migrate: %v", err)
	}
	if again := appliedVersions(t, pool); !slices.Equal(again, versions) {
		t.Errorf("the second migrate changed the applied versions: %v then %v", versions, again)
	}
}

// A fresh, empty database ends with the tables, the version recorded, the index and every fixed constraint name.
func TestMigration0034FromAnEmptyDatabase(t *testing.T) {
	ctx := context.Background()
	pool := dbtest.Pool(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	for _, table := range storefrontTables {
		if !tableExists(t, pool, table) {
			t.Fatalf("a fresh database has no %s after Migrate", table)
		}
	}
	if !slices.Contains(appliedVersions(t, pool), 34) {
		t.Errorf("a fresh database did not record version 34: %v", appliedVersions(t, pool))
	}
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM pg_indexes WHERE schemaname = current_schema() AND indexname = 'market_storefronts_by_node'`).Scan(&n); err != nil || n != 1 {
		t.Errorf("index market_storefronts_by_node: %d, %v; want 1", n, err)
	}
	for _, name := range storefrontConstraints {
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM pg_constraint WHERE connamespace = current_schema()::regnamespace AND conname = $1`, name).Scan(&n); err != nil || n != 1 {
			t.Errorf("constraint %s: %d, %v; want 1", name, n, err)
		}
	}
}

// Each table has exactly the columns migration 0034 names, with their types and nullability.
func TestTheStorefrontTablesHaveExactlyTheirColumns(t *testing.T) {
	ctx := context.Background()
	pool := dbtest.Pool(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	for table, want := range storefrontColumns {
		r, err := pool.Query(ctx, `SELECT column_name, data_type, is_nullable FROM information_schema.columns
			WHERE table_schema = current_schema() AND table_name = $1 ORDER BY ordinal_position`, table)
		if err != nil {
			t.Fatal(err)
		}
		var got []string
		for r.Next() {
			var name, dtype, nullable string
			if err := r.Scan(&name, &dtype, &nullable); err != nil {
				r.Close()
				t.Fatal(err)
			}
			got = append(got, name+" "+dtype+" "+nullable)
		}
		r.Close()
		if err := r.Err(); err != nil {
			t.Fatal(err)
		}
		if !slices.Equal(got, want) {
			t.Errorf("%s has columns\n got  %q\n want %q", table, got, want)
		}
	}
}

func TestStorefrontTablesConstraints(t *testing.T) {
	ctx := context.Background()
	pool := before34(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	front := func(node int64, character, text string, updated int64) error {
		_, err := pool.Exec(ctx, `INSERT INTO market_storefronts (node, character_id, description, updated_ms) VALUES ($1, $2, $3, $4)`, node, character, text, updated)
		return err
	}
	header := func(node int64, raises int64) error {
		_, err := pool.Exec(ctx, `INSERT INTO node_market_headers (node, header_raises) VALUES ($1, $2)`, node, raises)
		return err
	}
	// the limits of every column pass: one character, fifty, spaces at both ends kept, punctuation the recorded retail descriptions use, the largest count
	fifty := strings.Repeat("x", 50)
	for i, text := range []string{"x", fifty, "  Journeyman \\  Apprentice Sickle, Pickaxe, Axe  ", "ELG. I want Joeva to be trade hub. Vote for Me pls", "<b> & | ' ( ) * ! : ;"} {
		if err := front(int64(10+i), cybID, text, 1); err != nil {
			t.Errorf("a description %q: %v", text, err)
		}
	}
	if err := header(7, 9_000_000_000_000_000_000); err != nil {
		t.Errorf("a node with a large count: %v", err)
	}
	if err := header(8, 0); err != nil {
		t.Errorf("a node with the count 0: %v", err)
	}
	for _, tc := range []struct {
		name       string
		err        error
		code       string
		constraint string
	}{
		{"a node of 0", front(0, cybID, "x", 1), "23514", "market_storefronts_node"},
		{"an empty description", front(20, cybID, "", 1), "23514", "market_storefronts_length"},
		{"a description of 51 characters", front(21, cybID, fifty+"x", 1), "23514", "market_storefronts_length"},
		{"a new line", front(22, cybID, "one\ntwo", 1), "23514", "market_storefronts_plain"},
		{"a carriage return", front(23, cybID, "one\rtwo", 1), "23514", "market_storefronts_plain"},
		{"a tab", front(24, cybID, "one\ttwo", 1), "23514", "market_storefronts_plain"},
		{"a delete character", front(25, cybID, "one\x7ftwo", 1), "23514", "market_storefronts_plain"},
		{"a time of 0", front(26, cybID, "x", 0), "23514", "market_storefronts_updated"},
		{"a seller that is not there", front(27, "ffffffffffffffffffffffffffffffff", "x", 1), "23503", "market_storefronts_character_id_fkey"},
		{"a storefront twice", front(10, cybID, "again", 1), "23505", "market_storefronts_pkey"},
		{"a header node of 0", header(0, 0), "23514", "node_market_headers_node"},
		{"a negative count", header(9, -1), "23514", "node_market_headers_raises"},
		{"a header node twice", header(7, 1), "23505", "node_market_headers_pkey"},
	} {
		expectConstraint(t, tc.name, tc.err, tc.code, tc.constraint)
	}
	// a seller may keep a storefront at each of two markets
	if err := front(30, cybID, "one", 1); err != nil {
		t.Errorf("a storefront at node 30: %v", err)
	}
	if err := front(31, cybID, "two", 1); err != nil {
		t.Errorf("the same seller at node 31: %v", err)
	}
}

// The rows go with their seller: a deleted character takes its descriptions, and no other seller's.
func TestStorefrontRowsAreRemovedWithTheirSeller(t *testing.T) {
	ctx := context.Background()
	pool := before34(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO market_storefronts (node, character_id, description, updated_ms) VALUES (1, $1, 'mine', 1), (2, $1, 'mine too', 1)`, cybID); err != nil {
		t.Fatal(err)
	}
	var other string
	if err := pool.QueryRow(ctx, `SELECT id FROM characters WHERE id <> $1 LIMIT 1`, cybID).Scan(&other); err != nil {
		t.Fatalf("the play database has no second character: %v", err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO market_storefronts (node, character_id, description, updated_ms) VALUES (1, $1, 'theirs', 1)`, other); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `DELETE FROM characters WHERE id = $1`, cybID); err != nil {
		t.Fatal(err)
	}
	var mine, theirs int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM market_storefronts WHERE character_id = $1`, cybID).Scan(&mine); err != nil || mine != 0 {
		t.Errorf("after the character was deleted %d of its descriptions are left (%v), want none", mine, err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM market_storefronts WHERE character_id = $1`, other).Scan(&theirs); err != nil || theirs != 1 {
		t.Errorf("another seller's description: %d (%v), want 1", theirs, err)
	}
}

// The rollback in services/README.md ("Rolling back migration 0034"), run as written: the two tables and the record of the version go, nothing else changes, and the
// migration applies again.
func TestMigration0034RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0034"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	rollback := block[:strings.Index(block, "\n```")]

	pool := before34(t)
	schemaBefore := schemaWithoutStorefronts(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO market_storefronts (node, character_id, description, updated_ms) VALUES (1, $1, 'a storefront', 1)`, cybID); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO node_market_headers (node, header_raises) VALUES (1, 5)`); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("the README's rollback: %v", err)
	}
	for _, table := range storefrontTables {
		if tableExists(t, pool, table) {
			t.Errorf("%s is still there after the rollback", table)
		}
	}
	var recorded int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 34`).Scan(&recorded); err != nil || recorded != 0 {
		t.Errorf("version 34 is still recorded after the rollback (%d, %v)", recorded, err)
	}
	if after := schemaWithoutStorefronts(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Error("the rollback left the schema other than it was before 0034")
	}
	var chars int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM characters WHERE id = $1`, cybID).Scan(&chars); err != nil || chars != 1 {
		t.Errorf("the rollback removed the character (%d, %v)", chars, err)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate again after the rollback: %v", err)
	}
	for _, table := range storefrontTables {
		if !tableExists(t, pool, table) {
			t.Errorf("%s was not made again", table)
		}
	}
}
