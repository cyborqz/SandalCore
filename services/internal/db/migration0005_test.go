// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0005 (plan D1): the experience table and the GM starter set column. Tested on the real-shaped rows of
// migration0003_test.go and on what its constraints must refuse; the rollback is the README's block, run as written.

import (
	"context"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

// before5 is the schema as it is after plan B2 (0001-0004), with the real-shaped rows.
func before5(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool, _, _ := legacySetup(t)
	if err := migrate(context.Background(), pool, upTo(t, 5)); err != nil {
		t.Fatalf("migrate to 0004: %v", err)
	}
	return pool
}

func TestMigration0005AddsTheExperienceTableAndTheColumnAndTouchesNothingElse(t *testing.T) {
	ctx := context.Background()
	pool := before5(t)
	before := rows(t, pool, true)
	var had bool
	if err := pool.QueryRow(ctx, `SELECT to_regclass('character_experience') IS NOT NULL`).Scan(&had); err != nil || had {
		t.Fatalf("character_experience exists before 0005: %v %v", had, err)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	after := rows(t, pool, true)
	if len(after) != len(before) {
		t.Fatalf("%d characters after, %d before", len(after), len(before))
	}
	for id, b := range before {
		a := after[id]
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil
		if a != b {
			t.Errorf("%s changed by 0005", b.name)
		}
	}
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_experience`).Scan(&n); err != nil || n != 0 {
		t.Errorf("character_experience has %d rows after the migration (%v), want none", n, err)
	}
	// the column exists and is NULL for everyone: nothing was granted by a migration
	var granted int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM characters WHERE gm_starter_set_granted_at IS NOT NULL`).Scan(&granted); err != nil || granted != 0 {
		t.Errorf("%d characters are marked as given the starter set after the migration (%v), want none", granted, err)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Errorf("second migrate: %v", err)
	}
}

// Cyb x C.O.A

func TestCharacterExperienceConstraints(t *testing.T) {
	ctx := context.Background()
	pool := before5(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	insert := func(args ...any) error {
		_, err := pool.Exec(ctx, `INSERT INTO character_experience (character_id, leveling_type_guid, xp, debt) VALUES ($1, $2, $3, $4)`, args...)
		return err
	}
	if err := insert(cybID, 108651085518458, 418400, 0); err != nil {
		t.Fatalf("a good row: %v", err)
	}
	for _, tc := range []struct {
		name       string
		args       []any
		code       string
		constraint string
	}{
		{"the type is taken", []any{cybID, 108651085518458, 1, 0}, "23505", "character_experience_pkey"},
		{"no such character", []any{"ffffffffffffffffffffffffffffffff", 108651085518458, 1, 0}, "23503", "character_experience_character_id_fkey"},
		{"type 0", []any{cybID, 0, 1, 0}, "23514", "character_experience_type_positive"},
		{"negative xp", []any{cybID, 7, -1, 0}, "23514", "character_experience_xp"},
		{"negative debt", []any{cybID, 7, 1, -1}, "23514", "character_experience_debt"},
	} {
		err := insert(tc.args...)
		var pgErr *pgconn.PgError
		if err == nil {
			t.Errorf("%s: accepted", tc.name)
			continue
		}
		if !asPg(err, &pgErr) || pgErr.Code != tc.code || pgErr.ConstraintName != tc.constraint {
			t.Errorf("%s: %v, want %s on %s", tc.name, err, tc.code, tc.constraint)
		}
	}
	// the rows go with the character
	if _, err := pool.Exec(ctx, `DELETE FROM characters WHERE id = $1`, cybID); err != nil {
		t.Fatal(err)
	}
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_experience`).Scan(&n); err != nil || n != 0 {
		t.Errorf("%d experience rows of the deleted character remain (%v)", n, err)
	}
}

// The rollback in services/README.md ("Rolling back migration 0005"), run as written: the table and the column go, the
// record of the version too, the characters are as they were, and the migration applies again.
func TestMigration0005RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0005"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	rollback := block[:strings.Index(block, "\n```")]

	pool := before5(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	before := rows(t, pool, true)
	if _, err := pool.Exec(ctx, `INSERT INTO character_experience (character_id, leveling_type_guid, xp) VALUES ($1, 108651085518458, 300)`, cybID); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `UPDATE characters SET gm_starter_set_granted_at = now() WHERE id = $1`, cybID); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("the README's rollback: %v", err)
	}
	var has bool
	if err := pool.QueryRow(ctx, `SELECT to_regclass('character_experience') IS NOT NULL`).Scan(&has); err != nil || has {
		t.Errorf("character_experience still exists after the rollback (%v)", err)
	}
	// (this test's own schema: the other tests run in schemas of their own, in the same database)
	if err := pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'characters' AND column_name = 'gm_starter_set_granted_at')`).Scan(&has); err != nil || has {
		t.Errorf("the column still exists after the rollback (%v)", err)
	}
	var recorded int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 5`).Scan(&recorded); err != nil || recorded != 0 {
		t.Errorf("version 5 is still recorded after the rollback (%d, %v)", recorded, err)
	}
	after := rows(t, pool, true)
	for id, b := range before {
		a := after[id]
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil
		if a != b {
			t.Errorf("%s changed by the rollback", b.name)
		}
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate again after the rollback: %v", err)
	}
	if err := pool.QueryRow(ctx, `SELECT to_regclass('character_experience') IS NOT NULL`).Scan(&has); err != nil || !has {
		t.Errorf("character_experience is not back after migrating again (%v)", err)
	}
}
