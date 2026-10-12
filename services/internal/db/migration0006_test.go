// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0006 (plan D1, stage 2): the ranks a character bought and its action bar. Tested on the real-shaped rows of
// migration0003_test.go and on what its constraints must refuse; the rollback is the README's block, run as written. The 0005 file is
// not touched: the play database of stage 1 has it already, and 0006 only adds tables.

import (
	"context"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

// expectConstraint says whether err is the database refusing a row with the given SQLSTATE on the given constraint.
func expectConstraint(t *testing.T, name string, err error, code, constraint string) {
	t.Helper()
	var pgErr *pgconn.PgError
	if err == nil {
		t.Errorf("%s: accepted", name)
		return
	}
	if !asPg(err, &pgErr) || pgErr.Code != code || pgErr.ConstraintName != constraint {
		t.Errorf("%s: %v, want %s on %s", name, err, code, constraint)
	}
}

// before6 is the schema as it is after plan D1's stage 1 (0001-0005), with the real-shaped rows.
func before6(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool, _, _ := legacySetup(t)
	if err := migrate(context.Background(), pool, upTo(t, 6)); err != nil {
		t.Fatalf("migrate to 0005: %v", err)
	}
	return pool
}

// (CoA ~ C_Y_B)

func TestMigration0006AddsTheTalentTablesAndTouchesNothingElse(t *testing.T) {
	ctx := context.Background()
	pool := before6(t)
	before := rows(t, pool, true)
	for _, table := range []string{"character_skill_ranks", "character_action_bar"} {
		var had bool
		if err := pool.QueryRow(ctx, `SELECT to_regclass($1) IS NOT NULL`, table).Scan(&had); err != nil || had {
			t.Fatalf("%s exists before 0006: %v %v", table, had, err)
		}
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
			t.Errorf("%s changed by 0006", b.name)
		}
	}
	for _, table := range []string{"character_skill_ranks", "character_action_bar"} {
		var n int
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM `+table).Scan(&n); err != nil || n != 0 {
			t.Errorf("%s has %d rows after the migration (%v), want none", table, n, err)
		}
	}
	// 0005's own objects are as they were
	var has bool
	if err := pool.QueryRow(ctx, `SELECT to_regclass('character_experience') IS NOT NULL`).Scan(&has); err != nil || !has {
		t.Errorf("character_experience is gone after 0006 (%v)", err)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Errorf("second migrate: %v", err)
	}
}

func TestCharacterTalentConstraints(t *testing.T) {
	ctx := context.Background()
	pool := before6(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	rank := func(args ...any) error {
		_, err := pool.Exec(ctx, `INSERT INTO character_skill_ranks (character_id, tree_guid, ord, rank_guid) VALUES ($1, $2, $3, $4)`, args...)
		return err
	}
	if err := rank(cybID, 6064629885900499588, 0, 6064630736967303173); err != nil {
		t.Fatalf("a good rank: %v", err)
	}
	for _, tc := range []struct {
		name       string
		args       []any
		code       string
		constraint string
	}{
		{"the place is taken", []any{cybID, 6064629885900499588, 0, 5}, "23505", "character_skill_ranks_pkey"},
		{"the rank is in the tree already", []any{cybID, 6064629885900499588, 1, 6064630736967303173}, "23505", "character_skill_ranks_once"},
		{"no such character", []any{"ffffffffffffffffffffffffffffffff", 6064629885900499588, 0, 5}, "23503", "character_skill_ranks_character_id_fkey"},
		{"tree 0", []any{cybID, 0, 0, 5}, "23514", "character_skill_ranks_tree_positive"},
		{"rank 0", []any{cybID, 6064629885900499588, 1, 0}, "23514", "character_skill_ranks_rank_positive"},
		{"a negative place", []any{cybID, 6064629885900499588, -1, 5}, "23514", "character_skill_ranks_ord"},
	} {
		expectConstraint(t, tc.name, rank(tc.args...), tc.code, tc.constraint)
	}
	slot := func(args ...any) error {
		_, err := pool.Exec(ctx, `INSERT INTO character_action_bar (character_id, bank, slot, use_type, instigator, item_guid, item_rarity, item_quality, combo_button)
			VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)`, args...)
		return err
	}
	if err := slot(cybID, 0, 0, 2, 0, "", 0, 0, 0); err != nil {
		t.Fatalf("a good slot: %v", err)
	}
	if err := slot(cybID, 19, 15, 9, 3, strings.Repeat("ab", 16), 15, 200, 15); err != nil {
		t.Fatalf("a good slot at the limits: %v", err)
	}
	for _, tc := range []struct {
		name       string
		args       []any
		code       string
		constraint string
	}{
		{"the slot is taken", []any{cybID, 0, 0, 1, 0, "", 0, 0, 0}, "23505", "character_action_bar_pkey"},
		{"no such character", []any{"ffffffffffffffffffffffffffffffff", 0, 1, 1, 0, "", 0, 0, 0}, "23503", "character_action_bar_character_id_fkey"},
		{"bank 20", []any{cybID, 20, 1, 1, 0, "", 0, 0, 0}, "23514", "character_action_bar_bank"},
		{"bank -1", []any{cybID, -1, 1, 1, 0, "", 0, 0, 0}, "23514", "character_action_bar_bank"},
		{"slot 16", []any{cybID, 1, 16, 1, 0, "", 0, 0, 0}, "23514", "character_action_bar_slot"},
		{"use type 0", []any{cybID, 1, 1, 0, 0, "", 0, 0, 0}, "23514", "character_action_bar_use_type"},
		{"use type 10", []any{cybID, 1, 1, 10, 0, "", 0, 0, 0}, "23514", "character_action_bar_use_type"},
		{"instigator 4", []any{cybID, 1, 1, 1, 4, "", 0, 0, 0}, "23514", "character_action_bar_instigator"},
		{"an item guid that is not hex", []any{cybID, 1, 1, 3, 0, "XYZ", 0, 0, 0}, "23514", "character_action_bar_item_guid"},
		{"an upper-case item guid", []any{cybID, 1, 1, 3, 0, strings.Repeat("AB", 16), 0, 0, 0}, "23514", "character_action_bar_item_guid"},
		{"rarity 16", []any{cybID, 1, 1, 3, 0, "", 16, 0, 0}, "23514", "character_action_bar_rarity"},
		{"a negative quality", []any{cybID, 1, 1, 3, 0, "", 0, -1, 0}, "23514", "character_action_bar_quality"},
		{"combo 16", []any{cybID, 1, 1, 1, 0, "", 0, 0, 16}, "23514", "character_action_bar_combo"},
	} {
		expectConstraint(t, tc.name, slot(tc.args...), tc.code, tc.constraint)
	}
	// the rows go with the character
	if _, err := pool.Exec(ctx, `DELETE FROM characters WHERE id = $1`, cybID); err != nil {
		t.Fatal(err)
	}
	for _, table := range []string{"character_skill_ranks", "character_action_bar"} {
		var n int
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM `+table).Scan(&n); err != nil || n != 0 {
			t.Errorf("%d %s rows of the deleted character remain (%v)", n, table, err)
		}
	}
}

// The rollback in services/README.md ("Rolling back migration 0006"), run as written: the two tables and the record of the version go,
// 0005's table and the characters are as they were, and the migration applies again.
func TestMigration0006RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0006"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	rollback := block[:strings.Index(block, "\n```")]

	pool := before6(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	before := rows(t, pool, true)
	if _, err := pool.Exec(ctx, `INSERT INTO character_skill_ranks (character_id, tree_guid, ord, rank_guid) VALUES ($1, 6064629885900499588, 0, 5)`, cybID); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_action_bar (character_id, bank, slot, use_type) VALUES ($1, 0, 0, 1)`, cybID); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_experience (character_id, leveling_type_guid, xp) VALUES ($1, 108651085518458, 300)`, cybID); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("the README's rollback: %v", err)
	}
	for _, table := range []string{"character_skill_ranks", "character_action_bar"} {
		var has bool
		if err := pool.QueryRow(ctx, `SELECT to_regclass($1) IS NOT NULL`, table).Scan(&has); err != nil || has {
			t.Errorf("%s still exists after the rollback (%v)", table, err)
		}
	}
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_experience`).Scan(&n); err != nil || n != 1 {
		t.Errorf("0005's experience rows after the rollback: %d (%v), want the one row", n, err)
	}
	var recorded int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 6`).Scan(&recorded); err != nil || recorded != 0 {
		t.Errorf("version 6 is still recorded after the rollback (%d, %v)", recorded, err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 5`).Scan(&recorded); err != nil || recorded != 1 {
		t.Errorf("version 5 is not recorded after the rollback (%d, %v)", recorded, err)
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
	for _, table := range []string{"character_skill_ranks", "character_action_bar"} {
		var has bool
		if err := pool.QueryRow(ctx, `SELECT to_regclass($1) IS NOT NULL`, table).Scan(&has); err != nil || !has {
			t.Errorf("%s is not back after migrating again (%v)", table, err)
		}
	}
}
