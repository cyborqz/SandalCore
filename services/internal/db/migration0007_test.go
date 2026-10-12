// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0007 (the mounts milestone, stage 1): the state a mount item persists. It ADDS COLUMNS ONLY, so it is tested for what it must not do first:
// the items a play database already holds are read back exactly as they were (NULL, NULL and 0 in the new columns), a second run changes nothing, and
// the rollback is the README's block, run as written. The 0004 table is not edited: the play database has it already.

import (
	"context"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5/pgxpool"
)

// before7 is the schema as it is after 0006 (the play database of E1's stage runs), with the real-shaped characters and three items of cyb's.
func before7(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool, _, _ := legacySetup(t)
	if err := migrate(context.Background(), pool, upTo(t, 7)); err != nil {
		t.Fatalf("migrate to 0006: %v", err)
	}
	for i, row := range []struct {
		container string
		slot      int
		record    int64
		guid      string
		quantity  int
		quality   int
		durable   float32
	}{
		{"Backpack", 0, 6064630214426886486, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1", 1, 1, 11},
		{"Backpack", 69, 6064631121016127508, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa2", 1, 51, 0},
		{"Equipment", 2, 6064630214427083132, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa3", 5, 200, 37.5},
	} {
		if _, err := pool.Exec(context.Background(), `INSERT INTO character_items (character_id, container, slot, item_record_id, instance_guid, quantity, quality, durability, created_at)
			VALUES ($1, $2, $3, $4, $5, $6, $7, $8, to_timestamp(1760000000 + $9))`, cybID, row.container, row.slot, row.record, row.guid, row.quantity, row.quality, row.durable, i); err != nil {
			t.Fatalf("an item of the play database: %v", err)
		}
	}
	return pool
}

func itemSnapshot(t *testing.T, pool *pgxpool.Pool) []string {
	t.Helper()
	r, err := pool.Query(context.Background(), `SELECT character_id, container, slot, item_record_id, instance_guid, quantity, quality, durability::text, extract(epoch FROM created_at)::text
		FROM character_items ORDER BY container, slot`)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	var out []string
	for r.Next() {
		var a, b, d, f, g string
		var slot, quantity, quality int
		var record int64
		if err := r.Scan(&a, &b, &slot, &record, &d, &quantity, &quality, &f, &g); err != nil {
			t.Fatal(err)
		}
		out = append(out, strings.Join([]string{a, b, d, f, g}, "|"))
	}
	return out
}

// C O A x Cyb

func TestMigration0007AddsColumnsAndTouchesNothingElse(t *testing.T) {
	ctx := context.Background()
	pool := before7(t)
	chars := rows(t, pool, true)
	items := itemSnapshot(t, pool)
	if len(items) != 3 {
		t.Fatalf("the play database's items: %d", len(items))
	}
	for _, col := range []string{"persisted_health", "persisted_mana", "lock_expiration"} {
		var has bool
		if err := pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'character_items' AND column_name = $1)`, col).Scan(&has); err != nil || has {
			t.Fatalf("%s exists before 0007: %v %v", col, has, err)
		}
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	// every item reads as it did, and keeps nothing: NULL, NULL, 0
	if got := itemSnapshot(t, pool); strings.Join(got, "\n") != strings.Join(items, "\n") {
		t.Errorf("the items changed by 0007:\n%v\nwas\n%v", got, items)
	}
	var kept int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_items WHERE persisted_health IS NULL AND persisted_mana IS NULL AND lock_expiration = 0`).Scan(&kept); err != nil || kept != 3 {
		t.Errorf("%d of the 3 items keep nothing after the migration (%v), want all", kept, err)
	}
	after := rows(t, pool, true)
	if len(after) != len(chars) {
		t.Fatalf("%d characters after, %d before", len(after), len(chars))
	}
	for id, b := range chars {
		a := after[id]
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil // pointers: compared by value below
		if a != b {
			t.Errorf("%s changed by 0007", b.name)
		}
	}
	// the column types and defaults: nullable reals and a not-null bigint that defaults to 0
	for col, want := range map[string][3]string{
		"persisted_health": {"real", "YES", ""},
		"persisted_mana":   {"real", "YES", ""},
		"lock_expiration":  {"bigint", "NO", "0"},
	} {
		var typ, nullable string
		var def *string
		if err := pool.QueryRow(ctx, `SELECT data_type, is_nullable, column_default FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'character_items' AND column_name = $1`, col).Scan(&typ, &nullable, &def); err != nil {
			t.Fatal(err)
		}
		d := ""
		if def != nil {
			d = *def
		}
		if typ != want[0] || nullable != want[1] || d != want[2] {
			t.Errorf("%s is %s, nullable %s, default %q; want %v", col, typ, nullable, d, want)
		}
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Errorf("second migrate: %v", err)
	}
	if got := itemSnapshot(t, pool); strings.Join(got, "\n") != strings.Join(items, "\n") {
		t.Error("the second migrate changed the items")
	}
}

func TestItemPersistedStateConstraints(t *testing.T) {
	ctx := context.Background()
	pool := before7(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	slot := 10
	put := func(health, mana any, lock int64) error {
		slot++
		guid := strings.Repeat("c", 30) + string(rune('0'+slot/10)) + string(rune('0'+slot%10))
		_, err := pool.Exec(ctx, `INSERT INTO character_items (character_id, container, slot, item_record_id, instance_guid, persisted_health, persisted_mana, lock_expiration)
			VALUES ($1, 'Backpack', $2, 6064631121016127508, $3, $4, $5, $6)`, cybID, slot, guid, health, mana, lock)
		return err
	}
	// what a mount item keeps: Health 3572 and Mana 1 as retail's login shows, a lock time in the past, and the limits
	for name, args := range map[string]struct {
		health, mana any
		lock         int64
	}{
		"retail's mount":     {float32(3572), float32(1), 1770131735},
		"nothing kept":       {nil, nil, 0},
		"the limits":         {float32(0), float32(1e9), 253402300799},
		"only a lock":        {nil, nil, 1770131735},
		"a dead mount saved": {float32(0), float32(1), 1790000000},
	} {
		if err := put(args.health, args.mana, args.lock); err != nil {
			t.Errorf("%s: %v", name, err)
		}
	}
	for _, tc := range []struct {
		name         string
		health, mana any
		lock         int64
		constraint   string
	}{
		{"a negative health", float32(-1), nil, 0, "character_items_persisted_health"},
		{"a health above 1e9", float32(1.5e9), nil, 0, "character_items_persisted_health"},
		{"a health that is NaN", float32(nan32()), nil, 0, "character_items_persisted_health"},
		{"a negative mana", nil, float32(-0.5), 0, "character_items_persisted_mana"},
		{"a mana above 1e9", nil, float32(2e9), 0, "character_items_persisted_mana"},
		{"a negative lock", nil, nil, -1, "character_items_lock_expiration"},
		{"a lock past the year 9999", nil, nil, 253402300800, "character_items_lock_expiration"},
	} {
		expectConstraint(t, tc.name, put(tc.health, tc.mana, tc.lock), "23514", tc.constraint)
	}
}

func nan32() float32 {
	var zero float32
	return zero / zero
}

// The rollback in services/README.md ("Rolling back migration 0007"), run as written: the three columns, their constraints and the record of the
// version go, the items are as they were, and the migration applies again.
func TestMigration0007RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0007"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	rollback := block[:strings.Index(block, "\n```")]

	pool := before7(t)
	items := itemSnapshot(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `UPDATE character_items SET persisted_health = 3572, persisted_mana = 1, lock_expiration = 1770131735 WHERE slot = 69`); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("the README's rollback: %v", err)
	}
	for _, col := range []string{"persisted_health", "persisted_mana", "lock_expiration"} {
		var has bool
		if err := pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'character_items' AND column_name = $1)`, col).Scan(&has); err != nil || has {
			t.Errorf("%s is still there after the rollback (%v)", col, err)
		}
	}
	var recorded int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 7`).Scan(&recorded); err != nil || recorded != 0 {
		t.Errorf("version 7 is still recorded after the rollback (%d, %v)", recorded, err)
	}
	if got := itemSnapshot(t, pool); strings.Join(got, "\n") != strings.Join(items, "\n") {
		t.Errorf("the items after the rollback:\n%v\nwant\n%v", got, items)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate again after the rollback: %v", err)
	}
	var kept int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_items WHERE persisted_health IS NULL AND lock_expiration = 0`).Scan(&kept); err != nil || kept != 3 {
		t.Errorf("after migrating again %d of 3 items keep nothing (%v)", kept, err)
	}
}
