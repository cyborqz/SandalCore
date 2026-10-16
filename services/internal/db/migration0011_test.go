// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0011 (Gathering stage 3, "Resource storage"): the items of a character's resource containers. It ADDS ONE TABLE, character_resource_items, with the columns
// of character_items and its own constraints; the existing tables, columns and constraints (character_items' container CHECK among them) are NOT touched: every
// migration is add-only. It is tested for what it must not do first (every character and every item reads back as it was, no other table or column changes), then for
// what it makes (the constraints, the cascade), a second run changes nothing, and the rollback is the README's block, run as written.

import (
	"context"
	"os"
	"strconv"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5/pgxpool"
)

// before11 is the schema as it is after 0010 (the play database of the Gathering stage 2 test), with the real-shaped characters.
func before11(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool, _, _ := legacySetup(t)
	if err := migrate(context.Background(), pool, upTo(t, 11)); err != nil {
		t.Fatalf("migrate to 0010: %v", err)
	}
	return pool
}

// schemaWithout lists every column of every table but `skip`, with its type, nullability and default, and the constraints: what 0011 must not change.
func schemaWithout(t *testing.T, pool *pgxpool.Pool, skip string) []string {
	t.Helper()
	ctx := context.Background()
	var out []string
	for _, q := range []string{
		`SELECT table_name || '.' || column_name || ' ' || data_type || ' ' || is_nullable || ' ' || coalesce(column_default, '-') FROM information_schema.columns
			WHERE table_schema = current_schema() AND table_name <> $1 ORDER BY 1`,
		`SELECT conrelid::regclass::text || ' ' || conname || ' ' || pg_get_constraintdef(oid) FROM pg_constraint
			WHERE connamespace = current_schema()::regnamespace AND conrelid::regclass::text <> $1 ORDER BY 1`,
		`SELECT 'table ' || table_name FROM information_schema.tables WHERE table_schema = current_schema() AND table_name <> $1 ORDER BY 1`,
	} {
		r, err := pool.Query(ctx, q, skip)
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

// ~ C Y B :: C-O-A ~

func TestMigration0011AddsOneTableAndTouchesNothingElse(t *testing.T) {
	ctx := context.Background()
	pool := before11(t)
	chars := rows(t, pool, true)
	if len(chars) == 0 {
		t.Fatal("the play database has no characters")
	}
	if tableExists(t, pool, "character_resource_items") {
		t.Fatal("character_resource_items exists before 0011")
	}
	schemaBefore := schemaWithout(t, pool, "character_resource_items")
	var itemsBefore int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_items`).Scan(&itemsBefore); err != nil {
		t.Fatal(err)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	if !tableExists(t, pool, "character_resource_items") {
		t.Fatal("0011 did not make character_resource_items")
	}
	// nothing else changed: the same tables, columns, defaults and constraints (character_items' container CHECK is the one a wider design would have touched), the same items, every character as it was
	if after := schemaWithout(t, pool, "character_resource_items"); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Errorf("0011 changed more than character_resource_items:\nbefore %d entries, after %d", len(schemaBefore), len(after))
	}
	var itemsAfter int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_items`).Scan(&itemsAfter); err != nil || itemsAfter != itemsBefore {
		t.Errorf("character_items has %d rows after 0011 (%v), %d before", itemsAfter, err, itemsBefore)
	}
	var def string
	if err := pool.QueryRow(ctx, `SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'character_items_container'`).Scan(&def); err != nil || !strings.Contains(def, "'Backpack'") || strings.Contains(def, "Resource") {
		t.Errorf("character_items_container is %q (%v): it must stay as migration 0004 made it", def, err)
	}
	after := rows(t, pool, true)
	if len(after) != len(chars) {
		t.Fatalf("%d characters after, %d before", len(after), len(chars))
	}
	for id, b := range chars {
		a := after[id]
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil // pointers: compared by value
		if a != b {
			t.Errorf("%s changed by 0011", b.name)
		}
	}
	// the new table starts empty: nothing is backfilled (the login repair of worldd moves what a character holds in the Backpack)
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_resource_items`).Scan(&n); err != nil || n != 0 {
		t.Errorf("character_resource_items has %d rows after the migration (%v), want none", n, err)
	}

	// a row, and the constraints
	const guid = "0123456789abcdef0123456789abcdef"
	insert := func(container string, slot int, record int64, instance string, quantity, quality int, durability float64) error {
		_, err := pool.Exec(ctx, `INSERT INTO character_resource_items (character_id, container, slot, item_record_id, instance_guid, quantity, quality, durability)
			VALUES ($1, $2, $3, $4, $5, $6, $7, $8)`, cybID, container, slot, record, instance, quantity, quality, durability)
		return err
	}
	for _, ok := range []struct {
		container string
		slot      int
	}{{"Resource0", 0}, {"Resource0", 255}, {"Resource1", 39}, {"Resource2", 29}} {
		g := guid[:28] + strings.Repeat("0", 4-len(strconv.Itoa(ok.slot))) + strconv.Itoa(ok.slot)
		if err := insert(ok.container, ok.slot, 6064629382556287782, g, 20, 1, 0); err != nil {
			t.Errorf("%s slot %d: %v", ok.container, ok.slot, err)
		}
	}
	const taken = "ffffffffffffffffffffffffffffff01"
	if err := insert("Resource0", 7, 7, taken, 1, 1, 0); err != nil {
		t.Fatal(err)
	}
	for _, bad := range []struct {
		name  string
		err   error
		field string
	}{
		{"the slot is taken", insert("Resource0", 7, 7, "ffffffffffffffffffffffffffffff02", 1, 1, 0), "character_resource_items_pkey"},
		{"the guid is the character's already", insert("Resource1", 8, 7, taken, 1, 1, 0), "character_resource_items_guid_unique"},
		{"the Backpack", insert("Backpack", 3, 7, "ffffffffffffffffffffffffffffff03", 1, 1, 0), "character_resource_items_container"},
		{"a fourth resource container", insert("Resource3", 3, 7, "ffffffffffffffffffffffffffffff04", 1, 1, 0), "character_resource_items_container"},
		{"slot 256", insert("Resource0", 256, 7, "ffffffffffffffffffffffffffffff05", 1, 1, 0), "character_resource_items_slot"},
		{"slot -1", insert("Resource0", -1, 7, "ffffffffffffffffffffffffffffff06", 1, 1, 0), "character_resource_items_slot"},
		{"record 0", insert("Resource0", 9, 0, "ffffffffffffffffffffffffffffff07", 1, 1, 0), "character_resource_items_record_positive"},
		{"a guid in capitals", insert("Resource0", 9, 7, strings.ToUpper("ffffffffffffffffffffffffffffff08"), 1, 1, 0), "character_resource_items_guid_hex"},
		{"the zero guid", insert("Resource0", 9, 7, strings.Repeat("0", 32), 1, 1, 0), "character_resource_items_guid_hex"},
		{"quantity 0", insert("Resource0", 9, 7, "ffffffffffffffffffffffffffffff09", 0, 1, 0), "character_resource_items_quantity"},
		{"quantity past a million", insert("Resource0", 9, 7, "ffffffffffffffffffffffffffffff0a", 1000001, 1, 0), "character_resource_items_quantity"},
		{"quality 0", insert("Resource0", 9, 7, "ffffffffffffffffffffffffffffff0b", 1, 0, 0), "character_resource_items_quality"},
		{"quality 201", insert("Resource0", 9, 7, "ffffffffffffffffffffffffffffff0c", 1, 201, 0), "character_resource_items_quality"},
		{"a negative durability", insert("Resource0", 9, 7, "ffffffffffffffffffffffffffffff0d", 1, 1, -1), "character_resource_items_durability"},
	} {
		if bad.err == nil || !strings.Contains(bad.err.Error(), bad.field) {
			t.Errorf("%s: %v, want a violation of %s", bad.name, bad.err, bad.field)
		}
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_resource_items (character_id, container, slot, item_record_id, instance_guid) VALUES ('no such character', 'Resource0', 20, 7, 'ffffffffffffffffffffffffffffff0e')`); err == nil {
		t.Error("an item of a character that does not exist was accepted")
	}

	// a second run changes nothing
	var rowsNow int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_resource_items`).Scan(&rowsNow); err != nil {
		t.Fatal(err)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Errorf("second migrate: %v", err)
	}
	var rowsAfter int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_resource_items`).Scan(&rowsAfter); err != nil || rowsAfter != rowsNow {
		t.Errorf("after the second migrate %d rows (%v), %d before", rowsAfter, err, rowsNow)
	}
	// deleting a character takes its resource items along
	if _, err := pool.Exec(ctx, `DELETE FROM characters WHERE id = $1`, cybID); err != nil {
		t.Fatal(err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_resource_items WHERE character_id = $1`, cybID).Scan(&n); err != nil || n != 0 {
		t.Errorf("%d resource items outlive their character (%v)", n, err)
	}
}

// C.O.A.Cyb

// The rollback in services/README.md ("Rolling back migration 0011"), run as written: the table and the record of the version go, everything else is as it was, and the
// migration applies again.
func TestMigration0011RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0011"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	rollback := block[:strings.Index(block, "\n```")]

	pool := before11(t)
	chars := rows(t, pool, true)
	schemaBefore := schemaWithout(t, pool, "character_resource_items")
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_resource_items (character_id, container, slot, item_record_id, instance_guid, quantity) VALUES ($1, 'Resource0', 0, 7, 'eeeeeeeeeeeeeeeeeeeeeeeeeeeeee01', 3)`, cybID); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("the README's rollback: %v", err)
	}
	if tableExists(t, pool, "character_resource_items") {
		t.Error("character_resource_items is still there after the rollback")
	}
	var recorded int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 11`).Scan(&recorded); err != nil || recorded != 0 {
		t.Errorf("version 11 is still recorded after the rollback (%d, %v)", recorded, err)
	}
	if after := schemaWithout(t, pool, "character_resource_items"); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Error("the rollback left the schema other than it was before 0011")
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
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_resource_items`).Scan(&n); err != nil || n != 0 {
		t.Errorf("after migrating again %d rows (%v), want an empty table", n, err)
	}
}
