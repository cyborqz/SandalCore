// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0004 (plan B2): the items table. It runs on the real database when the services start, so it is tested on the
// rows shaped like the real ones (migration0003_test.go: cyb as the client wrote it) and on what its constraints must refuse.

import (
	"context"
	"math"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

const cybID = "bbec5c86eb101fe5f38c87f0fa22e7d0"

// before4 is the schema as it is in the real database after plan B1 (0001-0003), with the real-shaped rows.
func before4(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool, _, _ := legacySetup(t)
	if err := migrate(context.Background(), pool, upTo(t, 4)); err != nil {
		t.Fatalf("migrate to 0003: %v", err)
	}
	return pool
}

// C-O-A captures, Cyb code

func TestMigration0004AddsTheItemsTableAndTouchesNothingElse(t *testing.T) {
	ctx := context.Background()
	pool := before4(t)
	before := rows(t, pool, true)
	var had bool
	if err := pool.QueryRow(ctx, `SELECT to_regclass('character_items') IS NOT NULL`).Scan(&had); err != nil || had {
		t.Fatalf("character_items exists before 0004: %v %v", had, err)
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
		if !eq(a.race, b.race) || !eq(a.gender, b.gender) || !eq(a.class, b.class) {
			t.Errorf("%s: race %s gender %s class %s, was %s %s %s", b.name, show(a.race), show(a.gender), show(a.class), show(b.race), show(b.gender), show(b.class))
		}
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil
		if a != b {
			t.Errorf("%s changed by 0004", b.name)
		}
	}
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_items`).Scan(&n); err != nil || n != 0 {
		t.Errorf("character_items has %d rows after the migration (%v), want none: nothing is granted by a migration", n, err)
	}
	var versions []int
	r, err := pool.Query(ctx, `SELECT version FROM schema_migrations ORDER BY version`)
	if err != nil {
		t.Fatal(err)
	}
	for r.Next() {
		var v int
		if err := r.Scan(&v); err != nil {
			t.Fatal(err)
		}
		versions = append(versions, v)
	}
	r.Close()
	if len(versions) < 4 || versions[3] != 4 {
		t.Errorf("applied versions = %v, want 0004 recorded", versions)
	}
	// run again: nothing to do
	if err := Migrate(ctx, pool); err != nil {
		t.Errorf("second migrate: %v", err)
	}
	if again := rows(t, pool, true); len(again) != len(before) {
		t.Errorf("second migrate changed the characters")
	}
}

func TestCharacterItemsConstraints(t *testing.T) {
	ctx := context.Background()
	pool := before4(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	const guid = "0123456789abcdef0123456789abcdef"
	insert := func(args ...any) error {
		_, err := pool.Exec(ctx, `INSERT INTO character_items (character_id, container, slot, item_record_id, instance_guid, quantity, quality, durability)
			VALUES ($1, $2, $3, $4, $5, $6, $7, $8)`, args...)
		return err
	}
	// a good row, then the ways it can be wrong
	if err := insert(cybID, "Backpack", 69, 6064630214426886486, guid, 1, 1, 11.0); err != nil {
		t.Fatalf("a good item: %v", err)
	}
	for _, tc := range []struct {
		name       string
		args       []any
		code       string
		constraint string
	}{
		{"the slot is taken", []any{cybID, "Backpack", 69, 7, "11111111111111111111111111111111", 1, 1, 0.0}, "23505", "character_items_pkey"},
		{"the guid is the character's already", []any{cybID, "Backpack", 3, 7, guid, 1, 1, 0.0}, "23505", "character_items_guid_unique"},
		{"no such character", []any{"ffffffffffffffffffffffffffffffff", "Backpack", 3, 7, "22222222222222222222222222222222", 1, 1, 0.0}, "23503", "character_items_character_id_fkey"},
		{"no such storage", []any{cybID, "Bank", 3, 7, "33333333333333333333333333333333", 1, 1, 0.0}, "23514", "character_items_container"},
		{"Backpack slot 70", []any{cybID, "Backpack", 70, 7, "44444444444444444444444444444444", 1, 1, 0.0}, "23514", "character_items_slot"},
		{"Equipment slot 83", []any{cybID, "Equipment", 83, 7, "55555555555555555555555555555555", 1, 1, 0.0}, "23514", "character_items_slot"},
		{"Quest slot 100", []any{cybID, "Quest", 100, 7, "66666666666666666666666666666666", 1, 1, 0.0}, "23514", "character_items_slot"},
		{"slot -1", []any{cybID, "Quest", -1, 7, "77777777777777777777777777777777", 1, 1, 0.0}, "23514", "character_items_slot"},
		{"record 0", []any{cybID, "Backpack", 3, 0, "88888888888888888888888888888888", 1, 1, 0.0}, "23514", "character_items_record_positive"},
		{"a guid in capitals", []any{cybID, "Backpack", 3, 7, strings.ToUpper(guid), 1, 1, 0.0}, "23514", "character_items_guid_hex"},
		{"a guid of 31 digits", []any{cybID, "Backpack", 3, 7, guid[1:], 1, 1, 0.0}, "23514", "character_items_guid_hex"},
		{"the zero guid", []any{cybID, "Backpack", 3, 7, strings.Repeat("0", 32), 1, 1, 0.0}, "23514", "character_items_guid_hex"},
		{"quantity 0", []any{cybID, "Backpack", 3, 7, "99999999999999999999999999999999", 0, 1, 0.0}, "23514", "character_items_quantity"},
		{"quantity past a million", []any{cybID, "Backpack", 3, 7, "99999999999999999999999999999999", 1000001, 1, 0.0}, "23514", "character_items_quantity"},
		{"quality 0", []any{cybID, "Backpack", 3, 7, "99999999999999999999999999999999", 1, 0, 0.0}, "23514", "character_items_quality"},
		{"quality 201", []any{cybID, "Backpack", 3, 7, "99999999999999999999999999999999", 1, 201, 0.0}, "23514", "character_items_quality"},
		{"negative durability", []any{cybID, "Backpack", 3, 7, "99999999999999999999999999999999", 1, 1, -1.0}, "23514", "character_items_durability"},
		{"NaN durability", []any{cybID, "Backpack", 3, 7, "99999999999999999999999999999999", 1, 1, float32nan()}, "23514", "character_items_durability"},
		{"infinite durability", []any{cybID, "Backpack", 3, 7, "99999999999999999999999999999999", 1, 1, float32inf()}, "23514", "character_items_durability"},
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
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_items`).Scan(&n); err != nil || n != 1 {
		t.Errorf("%d items, want only the good one (%v)", n, err)
	}
	// the last slot of each storage is fine
	for _, tc := range []struct {
		container string
		slot      int
		guid      string
	}{{"Backpack", 0, "a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1"}, {"Equipment", 82, "b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2"}, {"Quest", 99, "c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3"}} {
		if err := insert(cybID, tc.container, tc.slot, 7, tc.guid, 1000000, 200, 1e9); err != nil {
			t.Errorf("%s slot %d at the limits: %v", tc.container, tc.slot, err)
		}
	}
}

// C:Y:B.CoA

func TestDeletingACharacterDeletesItsItems(t *testing.T) {
	ctx := context.Background()
	pool := before4(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	for i, id := range []string{cybID, "00000000000000000000000000000001"} {
		if _, err := pool.Exec(ctx, `INSERT INTO character_items (character_id, container, slot, item_record_id, instance_guid)
			VALUES ($1, 'Equipment', 2, 6064630214426886486, $2)`, id, strings.Repeat(string(rune('a'+i)), 32)); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := pool.Exec(ctx, `DELETE FROM characters WHERE id = $1`, cybID); err != nil {
		t.Fatal(err)
	}
	var mine, others int
	if err := pool.QueryRow(ctx, `SELECT count(*) FILTER (WHERE character_id = $1), count(*) FILTER (WHERE character_id <> $1) FROM character_items`, cybID).Scan(&mine, &others); err != nil {
		t.Fatal(err)
	}
	if mine != 0 || others != 1 {
		t.Errorf("after deleting cyb: %d of its items left, %d of the other's (want 0 and 1)", mine, others)
	}
}

func float32nan() float32 { return float32(math.NaN()) }
func float32inf() float32 { return float32(math.Inf(1)) }

// The rollback in services/README.md ("Rolling back migration 0004"), run as written: the items table and the record of the version
// go, the characters are as they were, and the migration applies again (to an empty table: a migration grants nothing).
func TestMigration0004RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0004"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	rollback := block[:strings.Index(block, "\n```")]

	pool := before4(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	var characters int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM characters`).Scan(&characters); err != nil || characters == 0 {
		t.Fatalf("%d characters before the rollback (%v)", characters, err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_items (character_id, container, slot, item_record_id, instance_guid)
		VALUES ($1, 'Equipment', 2, 6064630214426886486, $2)`, cybID, strings.Repeat("a", 32)); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("the README's rollback: %v", err)
	}
	var has bool
	var recorded, after int
	if err := pool.QueryRow(ctx, `SELECT to_regclass('character_items') IS NOT NULL`).Scan(&has); err != nil {
		t.Fatal(err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 4`).Scan(&recorded); err != nil {
		t.Fatal(err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM characters`).Scan(&after); err != nil {
		t.Fatal(err)
	}
	if has || recorded != 0 || after != characters {
		t.Errorf("after the rollback: table present %v, version 4 recorded %d times, %d characters (was %d)", has, recorded, after, characters)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("the migration after the rollback: %v", err)
	}
	var items int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_items`).Scan(&items); err != nil || items != 0 {
		t.Errorf("%d items after the migration is applied again (%v), want none", items, err)
	}
}
