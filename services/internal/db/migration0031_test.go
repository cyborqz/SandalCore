// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0031 (Marketplace sales). It ADDS THREE TABLES, character_mail_money, node_market_taxes and market_purchases, and touches nothing else (every migration is
// add-only). It is tested for what it must not do first (every character reads back as it was, no other table, column, constraint or index changes), then for what it makes
// (the exact columns and types, the keys and the fixed constraint names, the cascades), a second run changes nothing, and the rollback is the README's block, run as written,
// after which the migration applies again.

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

// salesTables are the three tables of migration 0031.
var salesTables = []string{"character_mail_money", "node_market_taxes", "market_purchases"}

// salesColumns is every column of the three tables, in the migration's order, with its type and nullability.
var salesColumns = map[string][]string{
	"character_mail_money": {"mail_id text NO", "copper bigint NO", "claimed boolean NO", "listing_id text YES", "sold_quantity integer NO"},
	"node_market_taxes":    {"node bigint NO", "rate real NO", "collected_copper bigint NO"},
	"market_purchases": {"purchase_id text NO", "listing_id text NO", "buyer_id text NO", "node bigint NO", "quantity integer NO", "price_copper bigint NO", "tax_copper bigint NO",
		"created_ms bigint NO"},
}

// salesConstraints is every constraint name migration 0031 fixes.
var salesConstraints = []string{
	"character_mail_money_pkey", "character_mail_money_mail_id_fkey", "character_mail_money_copper", "character_mail_money_listing_hex", "character_mail_money_listing_id_fkey",
	"character_mail_money_sold", "node_market_taxes_pkey", "node_market_taxes_node", "node_market_taxes_rate", "node_market_taxes_collected", "market_purchases_pkey",
	"market_purchases_id_hex", "market_purchases_node", "market_purchases_quantity", "market_purchases_price", "market_purchases_tax", "market_purchases_created",
	"market_purchases_listing_id_fkey", "market_purchases_buyer_id_fkey",
}

// before31 is the schema as it is at the migration before this one (0030, the listings), so 31 stops below this migration in this tree.
func before31(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool, _, _ := legacySetup(t)
	if err := migrate(context.Background(), pool, upTo(t, 31)); err != nil {
		t.Fatalf("migrate to 0030: %v", err)
	}
	return pool
}

// schemaWithoutSales lists every column, constraint, index and table but the ones of 0031: what 0031 must not change.
func schemaWithoutSales(t *testing.T, pool *pgxpool.Pool) []string {
	t.Helper()
	const skip = `('character_mail_money', 'node_market_taxes', 'market_purchases', 'character_trades', 'market_storefronts', 'node_market_headers')`
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

func TestMigration0031AddsThreeTablesAndTouchesNothingElse(t *testing.T) {
	ctx := context.Background()
	pool := before31(t)
	chars := rows(t, pool, true)
	if len(chars) == 0 {
		t.Fatal("the play database has no characters")
	}
	for _, table := range salesTables {
		if tableExists(t, pool, table) {
			t.Fatalf("%s exists before 0031", table)
		}
	}
	schemaBefore := schemaWithoutSales(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	for _, table := range salesTables {
		if !tableExists(t, pool, table) {
			t.Fatalf("0031 did not make %s", table)
		}
		var n int
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM `+table).Scan(&n); err != nil || n != 0 {
			t.Errorf("%s has %d rows after the migration (%v), want none: nothing is backfilled", table, n, err)
		}
	}
	if after := schemaWithoutSales(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Errorf("0031 changed more than its tables:\nbefore %d entries, after %d", len(schemaBefore), len(after))
	}
	after := rows(t, pool, true)
	if len(after) != len(chars) {
		t.Fatalf("%d characters after, %d before", len(after), len(chars))
	}
	for id, b := range chars {
		a := after[id]
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil // pointers: compared by value
		if a != b {
			t.Errorf("%s changed by 0031", b.name)
		}
	}
	versions := appliedVersions(t, pool)
	if !slices.Contains(versions, 31) {
		t.Errorf("applied versions = %v, want 31 in it", versions)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Errorf("second migrate: %v", err)
	}
	if again := appliedVersions(t, pool); !slices.Equal(again, versions) {
		t.Errorf("the second migrate changed the applied versions: %v then %v", versions, again)
	}
}

// A fresh, empty database ends with the tables, the version recorded, the indexes and every fixed constraint name.
func TestMigration0031FromAnEmptyDatabase(t *testing.T) {
	ctx := context.Background()
	pool := dbtest.Pool(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	for _, table := range salesTables {
		if !tableExists(t, pool, table) {
			t.Fatalf("a fresh database has no %s after Migrate", table)
		}
	}
	if !slices.Contains(appliedVersions(t, pool), 31) {
		t.Errorf("a fresh database did not record version 31: %v", appliedVersions(t, pool))
	}
	var n int
	for _, index := range []string{"character_mail_money_open_sale", "market_purchases_listing", "market_purchases_buyer"} {
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM pg_indexes WHERE schemaname = current_schema() AND indexname = $1`, index).Scan(&n); err != nil || n != 1 {
			t.Errorf("index %s: %d, %v; want 1", index, n, err)
		}
	}
	for _, name := range salesConstraints {
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM pg_constraint WHERE connamespace = current_schema()::regnamespace AND conname = $1`, name).Scan(&n); err != nil || n != 1 {
			t.Errorf("constraint %s: %d, %v; want 1", name, n, err)
		}
	}
}

// Each table has exactly the columns migration 0031 names, with their types and nullability.
func TestTheSalesTablesHaveExactlyTheirColumns(t *testing.T) {
	ctx := context.Background()
	pool := dbtest.Pool(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	for table, want := range salesColumns {
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

// seedSale makes a listing, a mail and its money for a test of the constraints.
func seedSale(t *testing.T, pool *pgxpool.Pool, n int) (listing, mail string) {
	t.Helper()
	hex := func(v int) string { return fmt.Sprintf("%032x", v) }
	listing, mail = hex(0xa000+n), hex(0xb000+n)
	if _, err := pool.Exec(context.Background(), `INSERT INTO character_market_listings (id, character_id, market_node, item_record_id, instance_guid, quantity, quality,
		durability, price_per_unit, status, issue_date_ms, expiry_date_ms) VALUES ($1, $2, 1, 7001, $3, 5, 1, 0, 100, 0, 1771000000000, 1771259200000)`, listing, cybID, hex(0xc000+n)); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(context.Background(), `INSERT INTO character_mails (id, character_id, sender, subject, body, status, issued_at_ms, expires_at_ms)
		VALUES ($1, $2, 'Marketplace', 'Item sold', 'x', 0, 1771000000000, 0)`, mail, cybID); err != nil {
		t.Fatal(err)
	}
	return listing, mail
}

func TestSalesTablesConstraints(t *testing.T) {
	ctx := context.Background()
	pool := before31(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	hex := func(n int) string { return fmt.Sprintf("%032x", n) }
	listing, mail := seedSale(t, pool, 1)
	_, mail2 := seedSale(t, pool, 2)
	money := func(mailID string, copper int64, claimed bool, listingID any, sold int) error {
		_, err := pool.Exec(ctx, `INSERT INTO character_mail_money (mail_id, copper, claimed, listing_id, sold_quantity) VALUES ($1, $2, $3, $4, $5)`, mailID, copper, claimed, listingID, sold)
		return err
	}
	if err := money(mail, 10_000_000_000_000, false, listing, 3); err != nil {
		t.Errorf("a mail money with the limits of every column: %v", err)
	}
	if err := money(mail2, 1, true, nil, 0); err != nil {
		t.Errorf("a claimed mail money of no listing: %v", err)
	}
	node := func(n int64, rate float32, collected int64) error {
		_, err := pool.Exec(ctx, `INSERT INTO node_market_taxes (node, rate, collected_copper) VALUES ($1, $2, $3)`, n, rate, collected)
		return err
	}
	for _, ok := range []float32{0.1, 0.5, 1} {
		if err := node(int64(100+ok*10), ok, 0); err != nil {
			t.Errorf("a node at the rate %v: %v", ok, err)
		}
	}
	if err := node(7, 0.5, 10_000_000_000); err != nil {
		t.Errorf("a node with a large total: %v", err)
	}
	purchase := func(id, listingID, buyer string, nodeID int64, quantity int, price, tax, created int64) error {
		_, err := pool.Exec(ctx, `INSERT INTO market_purchases (purchase_id, listing_id, buyer_id, node, quantity, price_copper, tax_copper, created_ms)
			VALUES ($1, $2, $3, $4, $5, $6, $7, $8)`, id, listingID, buyer, nodeID, quantity, price, tax, created)
		return err
	}
	if err := purchase(hex(0xd1), listing, cybID, 1, 1000000, 1, 0, 1); err != nil {
		t.Errorf("a purchase with the limits of every column: %v", err)
	}
	// each refusal below is tried on a mail of its own, so that the constraint it names is the first one it breaks
	spare := func(n int) string { _, m := seedSale(t, pool, n); return m }
	mailZero, mailNegative, mailCapitals, mailGhostListing, mailSecondOpen, mailOver := spare(3), spare(4), spare(5), spare(6), spare(7), spare(8)
	for _, tc := range []struct {
		name       string
		err        error
		code       string
		constraint string
	}{
		{"a money of 0", money(mailZero, 0, false, nil, 0), "23514", "character_mail_money_copper"},
		{"a money past the limit", money(mailOver, 10_000_000_000_001, false, nil, 0), "23514", "character_mail_money_copper"},
		{"a money of a mail that is not there", money(hex(0xb998), 5, false, nil, 0), "23503", "character_mail_money_mail_id_fkey"},
		{"the mail has money already", money(mail, 5, false, nil, 0), "23505", "character_mail_money_pkey"},
		{"a negative sold quantity", money(mailNegative, 5, false, nil, -1), "23514", "character_mail_money_sold"},
		{"a listing id in capitals", money(mailCapitals, 5, false, strings.ToUpper(hex(0xabc)), 0), "23514", "character_mail_money_listing_hex"},
		{"a listing that is not there", money(mailGhostListing, 5, false, hex(0xfeed), 0), "23503", "character_mail_money_listing_id_fkey"},
		{"a second unclaimed sale mail of one listing", money(mailSecondOpen, 5, false, listing, 1), "23505", "character_mail_money_open_sale"},
		{"a node of 0", node(0, 0.5, 0), "23514", "node_market_taxes_node"},
		{"a rate below the minimum", node(11, 0.09, 0), "23514", "node_market_taxes_rate"},
		{"a rate above the maximum", node(12, 1.01, 0), "23514", "node_market_taxes_rate"},
		{"a negative total", node(13, 0.5, -1), "23514", "node_market_taxes_collected"},
		{"a node twice", node(7, 0.5, 0), "23505", "node_market_taxes_pkey"},
		{"a purchase id of zeros", purchase(strings.Repeat("0", 32), listing, cybID, 1, 1, 1, 0, 1), "23514", "market_purchases_id_hex"},
		{"a purchase id in capitals", purchase(strings.ToUpper(hex(0xabc)), listing, cybID, 1, 1, 1, 0, 1), "23514", "market_purchases_id_hex"},
		{"a purchase twice", purchase(hex(0xd1), listing, cybID, 1, 1, 1, 0, 1), "23505", "market_purchases_pkey"},
		{"a purchase of a listing that is not there", purchase(hex(0xd2), hex(0xfeed), cybID, 1, 1, 1, 0, 1), "23503", "market_purchases_listing_id_fkey"},
		{"a purchase of a buyer that is not there", purchase(hex(0xd3), listing, "ffffffffffffffffffffffffffffffff", 1, 1, 1, 0, 1), "23503", "market_purchases_buyer_id_fkey"},
		{"a purchase at node 0", purchase(hex(0xd4), listing, cybID, 0, 1, 1, 0, 1), "23514", "market_purchases_node"},
		{"a purchase of 0 units", purchase(hex(0xd5), listing, cybID, 1, 0, 1, 0, 1), "23514", "market_purchases_quantity"},
		{"a purchase of too many units", purchase(hex(0xd6), listing, cybID, 1, 1000001, 1, 0, 1), "23514", "market_purchases_quantity"},
		{"a price of 0", purchase(hex(0xd7), listing, cybID, 1, 1, 0, 0, 1), "23514", "market_purchases_price"},
		{"a negative tax", purchase(hex(0xd8), listing, cybID, 1, 1, 1, -1, 1), "23514", "market_purchases_tax"},
		{"a purchase made at time 0", purchase(hex(0xd9), listing, cybID, 1, 1, 1, 0, 0), "23514", "market_purchases_created"},
	} {
		expectConstraint(t, tc.name, tc.err, tc.code, tc.constraint)
	}
	// a claimed sale mail does not block a new one for the same listing
	_, fresh := seedSale(t, pool, 20)
	if _, err := pool.Exec(ctx, `UPDATE character_mail_money SET claimed = true WHERE mail_id = $1`, mail); err != nil {
		t.Fatal(err)
	}
	if err := money(fresh, 5, false, listing, 1); err != nil {
		t.Errorf("a new sale mail for a listing whose earlier mail was claimed: %v", err)
	}
}

// The rows go with their owners: a deleted mail takes its money, a deleted character its mails' money and its purchases, and a deleted listing lets its sale mail go on
// (the listing_id is cleared).
func TestSalesRowsAreRemovedWithTheirOwners(t *testing.T) {
	ctx := context.Background()
	pool := before31(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	listing, mail := seedSale(t, pool, 1)
	if _, err := pool.Exec(ctx, `INSERT INTO character_mail_money (mail_id, copper, listing_id) VALUES ($1, 50, $2)`, mail, listing); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO market_purchases (purchase_id, listing_id, buyer_id, node, quantity, price_copper, tax_copper, created_ms)
		VALUES ('11112222333344445555666677778888', $1, $2, 1, 1, 50, 5, 1771000000000)`, listing, cybID); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `DELETE FROM character_market_listings WHERE id = $1`, listing); err != nil {
		t.Fatal(err)
	}
	var cleared, purchases int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_mail_money WHERE mail_id = $1 AND listing_id IS NULL`, mail).Scan(&cleared); err != nil || cleared != 1 {
		t.Errorf("after the listing was deleted the mail's money has its listing cleared: %d, %v; want 1", cleared, err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM market_purchases`).Scan(&purchases); err != nil || purchases != 0 {
		t.Errorf("after the listing was deleted %d purchases are left (%v), want none", purchases, err)
	}
	if _, err := pool.Exec(ctx, `DELETE FROM character_mails WHERE id = $1`, mail); err != nil {
		t.Fatal(err)
	}
	var left int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_mail_money`).Scan(&left); err != nil || left != 0 {
		t.Errorf("after the mail was deleted %d money rows are left (%v), want none", left, err)
	}
	_, mail2 := seedSale(t, pool, 2)
	if _, err := pool.Exec(ctx, `INSERT INTO character_mail_money (mail_id, copper) VALUES ($1, 9)`, mail2); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `DELETE FROM characters WHERE id = $1`, cybID); err != nil {
		t.Fatal(err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_mail_money`).Scan(&left); err != nil || left != 0 {
		t.Errorf("after the character was deleted %d money rows are left (%v), want none", left, err)
	}
}

// The rollback in services/README.md ("Rolling back migration 0031"), run as written: the three tables and the record of the version go, nothing else changes, and the
// migration applies again.
func TestMigration0031RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0031"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	rollback := block[:strings.Index(block, "\n```")]

	pool := before31(t)
	schemaBefore := schemaWithoutSales(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	listing, mail := seedSale(t, pool, 1)
	if _, err := pool.Exec(ctx, `INSERT INTO character_mail_money (mail_id, copper, listing_id) VALUES ($1, 50, $2)`, mail, listing); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO node_market_taxes (node, rate, collected_copper) VALUES (1, 0.5, 5)`); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("the README's rollback: %v", err)
	}
	for _, table := range salesTables {
		if tableExists(t, pool, table) {
			t.Errorf("%s is still there after the rollback", table)
		}
	}
	var recorded int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 31`).Scan(&recorded); err != nil || recorded != 0 {
		t.Errorf("version 31 is still recorded after the rollback (%d, %v)", recorded, err)
	}
	if after := schemaWithoutSales(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Error("the rollback left the schema other than it was before 0031")
	}
	// the mail and the listing the rows belonged to stay
	var mails, listings int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_mails WHERE id = $1`, mail).Scan(&mails); err != nil || mails != 1 {
		t.Errorf("the rollback removed the mail (%d, %v)", mails, err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_market_listings WHERE id = $1`, listing).Scan(&listings); err != nil || listings != 1 {
		t.Errorf("the rollback removed the listing (%d, %v)", listings, err)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate again after the rollback: %v", err)
	}
	for _, table := range salesTables {
		if !tableExists(t, pool, table) {
			t.Errorf("%s was not made again", table)
		}
	}
}

// rollBackLaterMigrations runs the README's rollback of 0031 (the only migration after 0030), which the older migrations' rollbacks need: its tables refer to their tables, so
// it goes first, as the README says.
func rollBackLaterMigrations(t *testing.T, pool *pgxpool.Pool) {
	t.Helper()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0031"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	if _, err := pool.Exec(context.Background(), block[:strings.Index(block, "\n```")]); err != nil {
		t.Fatalf("the README's rollback of 0031: %v", err)
	}
}
