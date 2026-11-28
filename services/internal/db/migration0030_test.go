// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0030 (Marketplace listings). It ADDS ONE TABLE, character_market_listings, and touches nothing else (every migration is add-only). It is tested for
// what it must not do first (every character reads back as it was, no other table, column, constraint or index changes), then for what it makes (the exact
// columns and types, the keys and the fixed constraint names, the cascade that removes a deleted character's listings), a second run changes nothing, and the
// rollback is the README's block, run as written, after which the migration applies again.

import (
	"context"
	"fmt"
	"os"
	"slices"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5/pgxpool"

	"aocrop/services/internal/dbtest"
)

const marketListingsTable = "character_market_listings"

// marketListingColumnNames is every column of the table, in the migration's order, and its type and nullability. A column that is not in the migration, or one
// the table holds without being here, fails TestTheMarketListingTableHasExactlyItsColumns.
var marketListingColumnNames = []string{
	"id text NO", "character_id text NO", "market_node bigint NO", "item_record_id bigint NO", "instance_guid text NO",
	"quantity integer NO", "quality integer NO", "durability real NO", "created_at timestamp with time zone NO",
	"persisted_health real YES", "persisted_mana real YES", "lock_expiration bigint NO", "price_per_unit bigint NO",
	"status integer NO", "issue_date_ms bigint NO", "expiry_date_ms bigint NO",
}

// marketListingConstraints is every constraint name migration 0030 fixes, so that the store and the tests can name them.
var marketListingConstraints = []string{
	"character_market_listings_id_hex", "character_market_listings_node", "character_market_listings_record_positive",
	"character_market_listings_guid_hex", "character_market_listings_quantity", "character_market_listings_quality",
	"character_market_listings_durability", "character_market_listings_price", "character_market_listings_status",
	"character_market_listings_issued", "character_market_listings_expires", "character_market_listings_persisted_health",
	"character_market_listings_persisted_mana", "character_market_listings_lock_expiration", "character_market_listings_pkey",
	"character_market_listings_character_id_fkey",
}

// before30 is the schema as it is at the migration before this one (0027, the mail), so 30 stops below this migration in this tree.
func before30(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool, _, _ := legacySetup(t)
	if err := migrate(context.Background(), pool, upTo(t, 30)); err != nil {
		t.Fatalf("migrate to 0027: %v", err)
	}
	return pool
}

// schemaWithoutMarketListings lists every column, constraint, index and table but the one of 0030: what 0030 must not change.
func schemaWithoutMarketListings(t *testing.T, pool *pgxpool.Pool) []string {
	t.Helper()
	const skip = `('character_market_listings', 'character_mail_money', 'node_market_taxes', 'market_purchases', 'character_trades', 'market_storefronts', 'node_market_headers')`
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

func TestMigration0030AddsTheMarketListingTableAndTouchesNothingElse(t *testing.T) {
	ctx := context.Background()
	pool := before30(t)
	chars := rows(t, pool, true)
	if len(chars) == 0 {
		t.Fatal("the play database has no characters")
	}
	if tableExists(t, pool, marketListingsTable) {
		t.Fatalf("%s exists before 0030", marketListingsTable)
	}
	schemaBefore := schemaWithoutMarketListings(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	if !tableExists(t, pool, marketListingsTable) {
		t.Fatalf("0030 did not make %s", marketListingsTable)
	}
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM `+marketListingsTable).Scan(&n); err != nil || n != 0 {
		t.Errorf("%s has %d rows after the migration (%v), want none: nothing is backfilled", marketListingsTable, n, err)
	}
	if after := schemaWithoutMarketListings(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Errorf("0030 changed more than its table:\nbefore %d entries, after %d", len(schemaBefore), len(after))
	}
	after := rows(t, pool, true)
	if len(after) != len(chars) {
		t.Fatalf("%d characters after, %d before", len(after), len(chars))
	}
	for id, b := range chars {
		a := after[id]
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil // pointers: compared by value
		if a != b {
			t.Errorf("%s changed by 0030", b.name)
		}
	}
	versions := appliedVersions(t, pool)
	if !slices.Contains(versions, 30) {
		t.Errorf("applied versions = %v, want 30 in it", versions)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Errorf("second migrate: %v", err)
	}
	if again := appliedVersions(t, pool); !slices.Equal(again, versions) {
		t.Errorf("the second migrate changed the applied versions: %v then %v", versions, again)
	}
}

// A fresh, empty database ends with the table, the version recorded, the read's index and every fixed constraint name.
func TestMigration0030FromAnEmptyDatabase(t *testing.T) {
	ctx := context.Background()
	pool := dbtest.Pool(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if !tableExists(t, pool, marketListingsTable) {
		t.Fatalf("a fresh database has no %s after Migrate", marketListingsTable)
	}
	if !slices.Contains(appliedVersions(t, pool), 30) {
		t.Errorf("a fresh database did not record version 30: %v", appliedVersions(t, pool))
	}
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM pg_indexes WHERE schemaname = current_schema() AND indexname = 'character_market_listings_listed'`).Scan(&n); err != nil || n != 1 {
		t.Errorf("the read's index: %d, %v; want 1", n, err)
	}
	for _, name := range marketListingConstraints {
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM pg_constraint WHERE connamespace = current_schema()::regnamespace AND conname = $1`, name).Scan(&n); err != nil || n != 1 {
			t.Errorf("constraint %s: %d, %v; want 1", name, n, err)
		}
	}
}

// The table has exactly the columns migration 0030 names, with their types and nullability.
func TestTheMarketListingTableHasExactlyItsColumns(t *testing.T) {
	ctx := context.Background()
	pool := dbtest.Pool(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	r, err := pool.Query(ctx, `SELECT column_name, data_type, is_nullable FROM information_schema.columns
		WHERE table_schema = current_schema() AND table_name = $1 ORDER BY ordinal_position`, marketListingsTable)
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
	if !slices.Equal(got, marketListingColumnNames) {
		t.Errorf("%s has columns\n got  %q\n want %q", marketListingsTable, got, marketListingColumnNames)
	}
}

func TestMarketListingTableConstraints(t *testing.T) {
	ctx := context.Background()
	pool := before30(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	hex := func(n int) string { return fmt.Sprintf("%032x", n) }
	put := func(id, character, itemGUID string, node, record int64, quantity, quality int, durability float64, price int64, status int, issued, expires int64) error {
		_, err := pool.Exec(ctx, `INSERT INTO character_market_listings (id, character_id, market_node, item_record_id, instance_guid, quantity, quality,
			durability, price_per_unit, status, issue_date_ms, expiry_date_ms) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)`,
			id, character, node, record, itemGUID, quantity, quality, durability, price, status, issued, expires)
		return err
	}
	listingA, listingB := hex(0xa1), hex(0xb2)
	if err := put(listingA, cybID, hex(0x11), 108646121231993, 6064630214426886486, 1000000, 200, 1e9, 1, 0, 1771000000000, 1771600000000); err != nil {
		t.Errorf("a listing with the limits of every column: %v", err)
	}
	if err := put(listingB, cybID, hex(0x12), 1, 7001, 1, 1, 0, 10000, 0, 1771000000001, 0); err != nil {
		t.Errorf("a listing with the smallest values: %v", err)
	}
	for _, tc := range []struct {
		name       string
		err        error
		code       string
		constraint string
	}{
		{"a listing id in capitals", put(strings.ToUpper(hex(0xabc)), cybID, hex(0x21), 1, 7001, 1, 1, 0, 1, 0, 1, 0), "23514", "character_market_listings_id_hex"},
		{"the zero listing id", put(strings.Repeat("0", 32), cybID, hex(0x21), 1, 7001, 1, 1, 0, 1, 0, 1, 0), "23514", "character_market_listings_id_hex"},
		{"a listing id that is not hex", put("not a guid", cybID, hex(0x21), 1, 7001, 1, 1, 0, 1, 0, 1, 0), "23514", "character_market_listings_id_hex"},
		{"a market node of 0", put(hex(0xc1), cybID, hex(0x21), 0, 7001, 1, 1, 0, 1, 0, 1, 0), "23514", "character_market_listings_node"},
		{"an item record of 0", put(hex(0xc2), cybID, hex(0x21), 1, 0, 1, 1, 0, 1, 0, 1, 0), "23514", "character_market_listings_record_positive"},
		{"an item guid in capitals", put(hex(0xc3), cybID, strings.ToUpper(hex(0xabc)), 1, 7001, 1, 1, 0, 1, 0, 1, 0), "23514", "character_market_listings_guid_hex"},
		{"the zero item guid", put(hex(0xc4), cybID, strings.Repeat("0", 32), 1, 7001, 1, 1, 0, 1, 0, 1, 0), "23514", "character_market_listings_guid_hex"},
		{"a quantity of 0", put(hex(0xc5), cybID, hex(0x21), 1, 7001, 0, 1, 0, 1, 0, 1, 0), "23514", "character_market_listings_quantity"},
		{"a quantity past the limit", put(hex(0xc6), cybID, hex(0x21), 1, 7001, 1000001, 1, 0, 1, 0, 1, 0), "23514", "character_market_listings_quantity"},
		{"a quality of 0", put(hex(0xc7), cybID, hex(0x21), 1, 7001, 1, 0, 0, 1, 0, 1, 0), "23514", "character_market_listings_quality"},
		{"a quality past the limit", put(hex(0xc8), cybID, hex(0x21), 1, 7001, 1, 201, 0, 1, 0, 1, 0), "23514", "character_market_listings_quality"},
		{"a negative durability", put(hex(0xc9), cybID, hex(0x21), 1, 7001, 1, 1, -1, 1, 0, 1, 0), "23514", "character_market_listings_durability"},
		{"a durability past the limit", put(hex(0xca), cybID, hex(0x21), 1, 7001, 1, 1, 2e9, 1, 0, 1, 0), "23514", "character_market_listings_durability"},
		{"a price of 0", put(hex(0xcb), cybID, hex(0x21), 1, 7001, 1, 1, 0, 0, 0, 1, 0), "23514", "character_market_listings_price"},
		{"a negative status", put(hex(0xcc), cybID, hex(0x21), 1, 7001, 1, 1, 0, 1, -1, 1, 0), "23514", "character_market_listings_status"},
		{"an issue time of 0", put(hex(0xcd), cybID, hex(0x21), 1, 7001, 1, 1, 0, 1, 0, 0, 0), "23514", "character_market_listings_issued"},
		{"a negative expiry", put(hex(0xce), cybID, hex(0x21), 1, 7001, 1, 1, 0, 1, 0, 1, -1), "23514", "character_market_listings_expires"},
		{"a listing of a character that does not exist", put(hex(0xcf), "ffffffffffffffffffffffffffffffff", hex(0x21), 1, 7001, 1, 1, 0, 1, 0, 1, 0), "23503", "character_market_listings_character_id_fkey"},
		{"a listing id twice", put(listingA, cybID, hex(0x22), 1, 7001, 1, 1, 0, 1, 0, 1, 0), "23505", "character_market_listings_pkey"},
	} {
		expectConstraint(t, tc.name, tc.err, tc.code, tc.constraint)
	}
	var listings int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_market_listings WHERE character_id = $1`, cybID).Scan(&listings); err != nil || listings != 2 {
		t.Errorf("cyb keeps %d listings (%v), want the 2 that were accepted", listings, err)
	}
}

// The listings go with their seller: deleting the character removes them and no other character's.
func TestMarketListingRowsAreRemovedWithTheirCharacter(t *testing.T) {
	ctx := context.Background()
	pool := before30(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	const other = "00000000000000000000000000000001"
	for n, id := range []string{cybID, other} {
		listingID := fmt.Sprintf("%032x", 0x5000+n)
		if _, err := pool.Exec(ctx, `INSERT INTO character_market_listings (id, character_id, market_node, item_record_id, instance_guid, quantity, quality,
			durability, price_per_unit, status, issue_date_ms, expiry_date_ms) VALUES ($1, $2, 1, 7001, $3, 1, 1, 0, 1, 0, 1771000000000, 0)`,
			listingID, id, fmt.Sprintf("%032x", 0x6000+n)); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := pool.Exec(ctx, `DELETE FROM characters WHERE id = $1`, cybID); err != nil {
		t.Fatal(err)
	}
	var mine, theirs int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_market_listings WHERE character_id = $1`, cybID).Scan(&mine); err != nil || mine != 0 {
		t.Errorf("a deleted character keeps %d listings (%v), want none", mine, err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_market_listings WHERE character_id = $1`, other).Scan(&theirs); err != nil || theirs != 1 {
		t.Errorf("another character keeps %d listings (%v), want its 1", theirs, err)
	}
}

// The rollback in services/README.md ("Rolling back migration 0030"), run as written: the table and the record of the version go, nothing else changes, and
// the migration applies again.
func TestMigration0030RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0030"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	rollback := block[:strings.Index(block, "\n```")]

	pool := before30(t)
	schemaBefore := schemaWithoutMarketListings(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_market_listings (id, character_id, market_node, item_record_id, instance_guid, quantity, quality,
		durability, price_per_unit, status, issue_date_ms, expiry_date_ms) VALUES
		('11112222333344445555666677778888', $1, 1, 7001, '0123456789abcdef0123456789abcdef', 1, 1, 0, 100, 0, 1771000000000, 0)`, cybID); err != nil {
		t.Fatal(err)
	}
	rollBackLaterMigrations(t, pool) // (0031 depends on this migration's tables: it goes first, as the README says)
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("the README's rollback: %v", err)
	}
	if tableExists(t, pool, marketListingsTable) {
		t.Errorf("%s is still there after the rollback", marketListingsTable)
	}
	var recorded int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 30`).Scan(&recorded); err != nil || recorded != 0 {
		t.Errorf("version 30 is still recorded after the rollback (%d, %v)", recorded, err)
	}
	if after := schemaWithoutMarketListings(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Error("the rollback left the schema other than it was before 0030")
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate again after the rollback: %v", err)
	}
	if !tableExists(t, pool, marketListingsTable) {
		t.Errorf("%s was not made again", marketListingsTable)
	}
}
