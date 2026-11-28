// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0032 (Player trading). It adds ONE TABLE, character_trades, and touches nothing else.

import (
	"context"
	"errors"
	"fmt"
	"slices"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"

	"aocrop/services/internal/dbtest"
)

const tradesTable = "character_trades"

var tradesColumns = []string{
	"id text NO",
	"char_a_id text NO",
	"char_b_id text NO",
	"copper_a_to_b bigint NO",
	"copper_b_to_a bigint NO",
	"items_a_to_b integer NO",
	"items_b_to_a integer NO",
	"committed_at_ms bigint NO",
}

var tradesConstraints = []string{
	"character_trades_pkey",
	"character_trades_id_hex",
	"character_trades_char_a_id_fkey",
	"character_trades_char_b_id_fkey",
	"character_trades_copper_a",
	"character_trades_copper_b",
	"character_trades_items_a",
	"character_trades_items_b",
	"character_trades_committed_at",
	"character_trades_distinct_chars",
	"character_trades_non_empty",
}

func before32(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool, _, _ := legacySetup(t)
	if err := migrate(context.Background(), pool, upTo(t, 32)); err != nil {
		t.Fatalf("migrate to 0031: %v", err)
	}
	return pool
}

func schemaWithoutTrades(t *testing.T, pool *pgxpool.Pool) []string {
	t.Helper()
	const skip = `('character_trades')`
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

func TestMigration0032AddsCharacterTradesAndTouchesNothingElse(t *testing.T) {
	ctx := context.Background()
	pool := before32(t)
	chars := rows(t, pool, true)
	if len(chars) == 0 {
		t.Fatal("the play database has no characters")
	}
	if tableExists(t, pool, tradesTable) {
		t.Fatalf("%s exists before 0032", tradesTable)
	}
	schemaBefore := schemaWithoutTrades(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	if !tableExists(t, pool, tradesTable) {
		t.Fatalf("0032 did not make %s", tradesTable)
	}
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_trades`).Scan(&n); err != nil || n != 0 {
		t.Errorf("character_trades has %d rows after migration (%v), want 0", n, err)
	}
	if after := schemaWithoutTrades(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Errorf("0032 changed more than its table: before %d entries, after %d", len(schemaBefore), len(after))
	}
	versions := appliedVersions(t, pool)
	if !slices.Contains(versions, 32) {
		t.Errorf("applied versions = %v, want 32 in it", versions)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Errorf("second migrate: %v", err)
	}
	if again := appliedVersions(t, pool); !slices.Equal(again, versions) {
		t.Errorf("second migrate changed applied versions: %v then %v", versions, again)
	}
}

func TestMigration0032FromAnEmptyDatabase(t *testing.T) {
	ctx := context.Background()
	pool := dbtest.Pool(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if !tableExists(t, pool, tradesTable) {
		t.Fatalf("fresh database has no %s after Migrate", tradesTable)
	}
	if !slices.Contains(appliedVersions(t, pool), 32) {
		t.Errorf("fresh database did not record version 32: %v", appliedVersions(t, pool))
	}
	r, err := pool.Query(ctx, `SELECT column_name, data_type, is_nullable FROM information_schema.columns
		WHERE table_schema = current_schema() AND table_name = $1 ORDER BY ordinal_position`, tradesTable)
	if err != nil {
		t.Fatal(err)
	}
	var gotCols []string
	for r.Next() {
		var name, dtype, nullable string
		if err := r.Scan(&name, &dtype, &nullable); err != nil {
			r.Close()
			t.Fatal(err)
		}
		gotCols = append(gotCols, name+" "+dtype+" "+nullable)
	}
	r.Close()
	if !slices.Equal(gotCols, tradesColumns) {
		t.Errorf("character_trades columns:\n got  %q\n want %q", gotCols, tradesColumns)
	}
	var n int
	for _, index := range []string{"character_trades_char_a_idx", "character_trades_char_b_idx"} {
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM pg_indexes WHERE schemaname = current_schema() AND indexname = $1`, index).Scan(&n); err != nil || n != 1 {
			t.Errorf("index %s: %d, %v; want 1", index, n, err)
		}
	}
	for _, name := range tradesConstraints {
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM pg_constraint WHERE connamespace = current_schema()::regnamespace AND conname = $1`, name).Scan(&n); err != nil || n != 1 {
			t.Errorf("constraint %s: %d, %v; want 1", name, n, err)
		}
	}
}

func TestMigration0032ConstraintsAndCascade(t *testing.T) {
	ctx := context.Background()
	pool := before32(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	hex := func(v int) string { return fmt.Sprintf("%032x", v) }
	// Pick a second distinct character from the seeded legacy DB.
	var otherID string
	if err := pool.QueryRow(ctx, `SELECT id FROM characters WHERE id <> $1 LIMIT 1`, cybID).Scan(&otherID); err != nil {
		t.Fatalf("need a second character: %v", err)
	}

	insert := func(id, a, b string, ca, cb int64, ia, ib int, at int64) error {
		_, err := pool.Exec(ctx, `INSERT INTO character_trades
			(id, char_a_id, char_b_id, copper_a_to_b, copper_b_to_a, items_a_to_b, items_b_to_a, committed_at_ms)
			VALUES ($1, $2, $3, $4, $5, $6, $7, $8)`, id, a, b, ca, cb, ia, ib, at)
		return err
	}

	if err := insert(hex(0x101), cybID, otherID, 10000000, 0, 1, 0, 1771000000000); err != nil {
		t.Fatalf("valid trade row rejected: %v", err)
	}

	cases := []struct {
		name       string
		err        error
		code       string
		constraint string
	}{
		{"all-zero id", insert(strings.Repeat("0", 32), cybID, otherID, 1, 0, 0, 0, 1), "23514", "character_trades_id_hex"},
		{"uppercase id", insert(strings.ToUpper(hex(0xabc)), cybID, otherID, 1, 0, 0, 0, 1), "23514", "character_trades_id_hex"},
		{"duplicate id", insert(hex(0x101), cybID, otherID, 1, 0, 0, 0, 1), "23505", "character_trades_pkey"},
		{"unknown char_a", insert(hex(0x102), hex(0xdead), otherID, 1, 0, 0, 0, 1), "23503", "character_trades_char_a_id_fkey"},
		{"unknown char_b", insert(hex(0x103), cybID, hex(0xbeef), 1, 0, 0, 0, 1), "23503", "character_trades_char_b_id_fkey"},
		{"same character", insert(hex(0x104), cybID, cybID, 1, 0, 0, 0, 1), "23514", "character_trades_distinct_chars"},
		{"empty trade", insert(hex(0x105), cybID, otherID, 0, 0, 0, 0, 1), "23514", "character_trades_non_empty"},
		{"negative copper_a", insert(hex(0x106), cybID, otherID, -1, 0, 1, 0, 1), "23514", "character_trades_copper_a"},
		{"over-cap copper_a", insert(hex(0x107), cybID, otherID, 10000001, 0, 0, 0, 1), "23514", "character_trades_copper_a"},
		{"over-cap copper_b", insert(hex(0x108), cybID, otherID, 0, 10000001, 0, 0, 1), "23514", "character_trades_copper_b"},
		{"negative items_a", insert(hex(0x109), cybID, otherID, 1, 0, -1, 0, 1), "23514", "character_trades_items_a"},
		{"negative items_b", insert(hex(0x10a), cybID, otherID, 1, 0, 0, -1, 1), "23514", "character_trades_items_b"},
		{"zero timestamp", insert(hex(0x10b), cybID, otherID, 1, 0, 0, 0, 0), "23514", "character_trades_committed_at"},
	}
	for _, tc := range cases {
		var pg *pgconn.PgError
		if !errors.As(tc.err, &pg) || pg.Code != tc.code || pg.ConstraintName != tc.constraint {
			t.Errorf("%s: got %v, want pg code %s constraint %s", tc.name, tc.err, tc.code, tc.constraint)
		}
	}

	// Deleting either participant cascades and removes the trade row.
	if _, err := pool.Exec(ctx, `DELETE FROM characters WHERE id = $1`, otherID); err != nil {
		t.Fatal(err)
	}
	var left int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_trades`).Scan(&left); err != nil || left != 0 {
		t.Errorf("after character delete %d trade rows remain (%v), want 0", left, err)
	}
}

func TestMigration0032RollbackAndReapply(t *testing.T) {
	ctx := context.Background()
	const rollback = `
BEGIN;
DROP TABLE IF EXISTS character_trades;
DELETE FROM schema_migrations WHERE version = 32;
COMMIT;
`
	pool := before32(t)
	schemaBefore := schemaWithoutTrades(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("rollback 0032: %v", err)
	}
	if tableExists(t, pool, tradesTable) {
		t.Errorf("%s still exists after rollback", tradesTable)
	}
	var recorded int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 32`).Scan(&recorded); err != nil || recorded != 0 {
		t.Errorf("version 32 still recorded after rollback (%d, %v)", recorded, err)
	}
	if after := schemaWithoutTrades(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Error("rollback left schema different from before 0032")
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("reapply 0032: %v", err)
	}
	if !tableExists(t, pool, tradesTable) {
		t.Errorf("%s not recreated on reapply", tradesTable)
	}
}
