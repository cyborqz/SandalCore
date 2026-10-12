// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0008 (the mounts milestone, stage 3): one nullable column on characters, gm_starter_mounts_granted_at.  It ADDS A COLUMN ONLY, so it is tested for what it must
// not do first (every character reads back exactly as it was, with NULL in the new column), a second run changes nothing, and the rollback is the README's block, run as
// written (review B I3: 0003 to 0007 had such a test, 0008 had none).

import (
	"context"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5/pgxpool"
)

// before8 is the schema as it is after 0007 (the play database of the mounts stage runs), with the real-shaped characters.
func before8(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool, _, _ := legacySetup(t)
	if err := migrate(context.Background(), pool, upTo(t, 8)); err != nil {
		t.Fatalf("migrate to 0007: %v", err)
	}
	return pool
}

func hasMountsColumn(t *testing.T, pool *pgxpool.Pool) bool {
	t.Helper()
	var has bool
	if err := pool.QueryRow(context.Background(), `SELECT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'characters' AND column_name = 'gm_starter_mounts_granted_at')`).Scan(&has); err != nil {
		t.Fatal(err)
	}
	return has
}

func TestMigration0008AddsOneNullableColumnAndTouchesNothingElse(t *testing.T) {
	ctx := context.Background()
	pool := before8(t)
	chars := rows(t, pool, true)
	if len(chars) == 0 {
		t.Fatal("the play database has no characters")
	}
	if hasMountsColumn(t, pool) {
		t.Fatal("gm_starter_mounts_granted_at exists before 0008")
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	if !hasMountsColumn(t, pool) {
		t.Fatal("gm_starter_mounts_granted_at is missing after 0008")
	}
	after := rows(t, pool, true)
	if len(after) != len(chars) {
		t.Fatalf("%d characters after, %d before", len(after), len(chars))
	}
	for id, b := range chars {
		a := after[id]
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil // pointers: compared by value
		if a != b {
			t.Errorf("%s changed by 0008", b.name)
		}
	}
	// the column: a nullable timestamptz that no row has set
	var typ, nullable string
	var def *string
	if err := pool.QueryRow(ctx, `SELECT data_type, is_nullable, column_default FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'characters' AND column_name = 'gm_starter_mounts_granted_at'`).Scan(&typ, &nullable, &def); err != nil {
		t.Fatal(err)
	}
	if typ != "timestamp with time zone" || nullable != "YES" || def != nil {
		t.Errorf("the column is %s, nullable %s, default %v; want a nullable timestamptz with no default", typ, nullable, def)
	}
	var set int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM characters WHERE gm_starter_mounts_granted_at IS NOT NULL`).Scan(&set); err != nil || set != 0 {
		t.Errorf("%d characters have the column set by the migration (%v), want none", set, err)
	}
	// it is written and read back, and a second run changes nothing
	if _, err := pool.Exec(ctx, `UPDATE characters SET gm_starter_mounts_granted_at = now() WHERE id = $1`, cybID); err != nil {
		t.Fatalf("the column does not take a time: %v", err)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Errorf("second migrate: %v", err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM characters WHERE gm_starter_mounts_granted_at IS NOT NULL`).Scan(&set); err != nil || set != 1 {
		t.Errorf("after the second migrate %d characters have the column set (%v), want the one", set, err)
	}
}

// [C-O-A ~ C:Y:B]

// The rollback in services/README.md ("Rolling back migration 0008"), run as written: the column and the record of the version go, the characters are as they were,
// and the migration applies again.
func TestMigration0008RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0008"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	rollback := block[:strings.Index(block, "\n```")]

	pool := before8(t)
	chars := rows(t, pool, true)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `UPDATE characters SET gm_starter_mounts_granted_at = now() WHERE id = $1`, cybID); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("the README's rollback: %v", err)
	}
	if hasMountsColumn(t, pool) {
		t.Error("gm_starter_mounts_granted_at is still there after the rollback")
	}
	var recorded int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 8`).Scan(&recorded); err != nil || recorded != 0 {
		t.Errorf("version 8 is still recorded after the rollback (%d, %v)", recorded, err)
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
	if !hasMountsColumn(t, pool) {
		t.Error("the column is missing after migrating again")
	}
}
