// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0027 (mail). It ADDS TWO TABLES, character_mails and character_mail_items, and touches nothing else (every migration is add-only). It is tested for
// what it must not do first (every character reads back as it was, no other table, column, constraint or index changes), then for what it makes (the exact
// columns and types, no money column, the keys and the fixed constraint names, the cascade that removes a deleted character's mails and their items), a
// second run changes nothing, and the rollback is the README's block, run as written, after which the migration applies again.

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

var mailTables = []string{"character_mails", "character_mail_items"}

// mailColumnNames is every column of the two tables, in the migration's order, and its type and nullability. A column that is not in the migration, or one
// the table holds without being here, fails TestTheMailTablesHaveExactlyTheirColumns; no money column can pass it.
var mailColumnNames = map[string][]string{
	"character_mails": {
		"id text NO", "character_id text NO", "sender text NO", "subject text NO", "body text NO",
		"status integer NO", "issued_at_ms bigint NO", "expires_at_ms bigint NO",
	},
	"character_mail_items": {
		"mail_id text NO", "position integer NO", "item_record_id bigint NO", "instance_guid text NO",
		"quantity integer NO", "quality integer NO", "durability real NO", "created_at timestamp with time zone NO",
		"persisted_health real YES", "persisted_mana real YES", "lock_expiration bigint NO", "claimed boolean NO",
	},
}

// mailConstraints is every constraint name migration 0027 fixes, so that the store and the tests can name them.
var mailConstraints = []string{
	"character_mails_id_hex", "character_mails_sender", "character_mails_subject", "character_mails_body",
	"character_mails_status", "character_mails_issued", "character_mails_expires", "character_mails_pkey",
	"character_mails_character_id_fkey",
	"character_mail_items_position", "character_mail_items_record_positive", "character_mail_items_guid_hex",
	"character_mail_items_quantity", "character_mail_items_quality", "character_mail_items_durability",
	"character_mail_items_persisted_health", "character_mail_items_persisted_mana", "character_mail_items_lock_expiration",
	"character_mail_items_pkey", "character_mail_items_guid_unique", "character_mail_items_mail_id_fkey",
}

// before27 is the schema as it is at the migration before this one (0026, the guilds), with the real-shaped characters. upTo takes the versions below its
// limit, so 27 stops below this migration, at 0026 in this tree.
func before27(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool, _, _ := legacySetup(t)
	if err := migrate(context.Background(), pool, upTo(t, 27)); err != nil {
		t.Fatalf("migrate to 0026: %v", err)
	}
	return pool
}

// schemaWithoutMails lists every column, constraint, index and table but the two of 0027: what 0027 must not change.
func schemaWithoutMails(t *testing.T, pool *pgxpool.Pool) []string {
	t.Helper()
	const skip = `('character_mails', 'character_mail_items')`
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

func TestMigration0027AddsTheMailTablesAndTouchesNothingElse(t *testing.T) {
	ctx := context.Background()
	pool := before27(t)
	chars := rows(t, pool, true)
	if len(chars) == 0 {
		t.Fatal("the play database has no characters")
	}
	for _, tb := range mailTables {
		if tableExists(t, pool, tb) {
			t.Fatalf("%s exists before 0027", tb)
		}
	}
	schemaBefore := schemaWithoutMails(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	for _, tb := range mailTables {
		if !tableExists(t, pool, tb) {
			t.Fatalf("0027 did not make %s", tb)
		}
		var n int
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM `+tb).Scan(&n); err != nil || n != 0 {
			t.Errorf("%s has %d rows after the migration (%v), want none: nothing is backfilled", tb, n, err)
		}
	}
	if after := schemaWithoutMails(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Errorf("0027 changed more than its two tables:\nbefore %d entries, after %d", len(schemaBefore), len(after))
	}
	after := rows(t, pool, true)
	if len(after) != len(chars) {
		t.Fatalf("%d characters after, %d before", len(after), len(chars))
	}
	for id, b := range chars {
		a := after[id]
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil // pointers: compared by value
		if a != b {
			t.Errorf("%s changed by 0027", b.name)
		}
	}
	versions := appliedVersions(t, pool)
	if !slices.Contains(versions, 27) {
		t.Errorf("applied versions = %v, want 27 in it", versions)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Errorf("second migrate: %v", err)
	}
	if again := appliedVersions(t, pool); !slices.Equal(again, versions) {
		t.Errorf("the second migrate changed the applied versions: %v then %v", versions, again)
	}
}

// A fresh, empty database ends with both tables, the version recorded, the read's index and every fixed constraint name.
func TestMigration0027FromAnEmptyDatabase(t *testing.T) {
	ctx := context.Background()
	pool := dbtest.Pool(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	for _, tb := range mailTables {
		if !tableExists(t, pool, tb) {
			t.Fatalf("a fresh database has no %s after Migrate", tb)
		}
	}
	if !slices.Contains(appliedVersions(t, pool), 27) {
		t.Errorf("a fresh database did not record version 27: %v", appliedVersions(t, pool))
	}
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM pg_indexes WHERE schemaname = current_schema() AND indexname = 'character_mails_character'`).Scan(&n); err != nil || n != 1 {
		t.Errorf("the read's index: %d, %v; want 1", n, err)
	}
	for _, name := range mailConstraints {
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM pg_constraint WHERE connamespace = current_schema()::regnamespace AND conname = $1`, name).Scan(&n); err != nil || n != 1 {
			t.Errorf("constraint %s: %d, %v; want 1", name, n, err)
		}
	}
}

// The two tables have exactly the columns migration 0027 names, with their types and nullability: no money column and nothing else.
func TestTheMailTablesHaveExactlyTheirColumns(t *testing.T) {
	ctx := context.Background()
	pool := dbtest.Pool(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	for table, want := range mailColumnNames {
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
	var money int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM information_schema.columns WHERE table_schema = current_schema()
		AND table_name IN ('character_mails', 'character_mail_items') AND (column_name LIKE '%currency%' OR column_name LIKE '%money%' OR column_name LIKE '%coin%')`).Scan(&money); err != nil || money != 0 {
		t.Errorf("the mail tables hold %d money columns (%v), want none: only items are attached", money, err)
	}
}

func TestMailTablesConstraints(t *testing.T) {
	ctx := context.Background()
	pool := before27(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	hex := func(n int) string { return fmt.Sprintf("%032x", n) }
	mail := func(id, character, sender, subject, body string, status int, issued, expires int64) error {
		_, err := pool.Exec(ctx, `INSERT INTO character_mails (id, character_id, sender, subject, body, status, issued_at_ms, expires_at_ms)
			VALUES ($1, $2, $3, $4, $5, $6, $7, $8)`, id, character, sender, subject, body, status, issued, expires)
		return err
	}
	put := func(mailID, guid string, position int, record int64, quantity, quality int, durability float64, health, mana *float64, lock int64, claimed bool) error {
		_, err := pool.Exec(ctx, `INSERT INTO character_mail_items (mail_id, position, item_record_id, instance_guid, quantity, quality, durability,
			persisted_health, persisted_mana, lock_expiration, claimed) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11)`,
			mailID, position, record, guid, quantity, quality, durability, health, mana, lock, claimed)
		return err
	}
	mailA, mailB := hex(0xa1), hex(0xb2)
	item := func(mailID, guid string, position int) error {
		return put(mailID, guid, position, 7001, 1, 1, 11, nil, nil, 0, false)
	}
	// what a mail and its items are: every field, an empty body, the longest text, a letter outside ASCII at the byte limit, NULL persisted stats, a flag
	long := strings.Repeat("n", 64)
	ascii := strings.Repeat("é", 32) // 64 bytes
	subjectLong := strings.Repeat("s", 128)
	bodyLong := strings.Repeat("b", 4096)
	for _, tc := range []struct {
		name string
		err  error
	}{
		{"a mail with every field", mail(mailA, cybID, long, subjectLong, bodyLong, 6, 1771000000000, 1771600000000)},
		{"a mail with no items and no expiry", mail(mailB, cybID, ascii, "Hello", "", 0, 1771000000001, 0)},
		{"an item with every field", put(mailA, hex(1), 0, 6064630214426886486, 1000000, 200, 1e9, ptrF(1e9), ptrF(0), 253402300799, true)},
		{"an item with no persisted state", item(mailA, hex(2), 1)},
		{"an item without a claimed flag set", put(mailA, hex(3), 2, 7002, 1, 1, 0, nil, nil, 0, false)},
		{"the same item guid in another mail", item(mailB, hex(1), 0)},
		{"a third item on the second mail", item(mailB, hex(4), 1)},
	} {
		if tc.err != nil {
			t.Errorf("%s: %v", tc.name, tc.err)
		}
	}
	for _, tc := range []struct {
		name       string
		err        error
		code       string
		constraint string
	}{
		{"a mail id in capitals", mail(strings.ToUpper(hex(0xabc)), cybID, "Brannoc", "Hello", "", 0, 1, 0), "23514", "character_mails_id_hex"},
		{"the zero mail id", mail(strings.Repeat("0", 32), cybID, "Brannoc", "Hello", "", 0, 1, 0), "23514", "character_mails_id_hex"},
		{"a mail id that is not hex", mail("not a guid", cybID, "Brannoc", "Hello", "", 0, 1, 0), "23514", "character_mails_id_hex"},
		{"an empty sender", mail(hex(0xc1), cybID, "", "Hello", "", 0, 1, 0), "23514", "character_mails_sender"},
		{"a sender with a space in front", mail(hex(0xc2), cybID, " Brannoc", "Hello", "", 0, 1, 0), "23514", "character_mails_sender"},
		{"a sender behind the byte limit", mail(hex(0xc3), cybID, strings.Repeat("é", 33), "Hello", "", 0, 1, 0), "23514", "character_mails_sender"},
		{"an empty subject", mail(hex(0xc4), cybID, "Brannoc", "", "", 0, 1, 0), "23514", "character_mails_subject"},
		{"a subject with a space behind", mail(hex(0xc5), cybID, "Brannoc", "Hello ", "", 0, 1, 0), "23514", "character_mails_subject"},
		{"a subject behind the byte limit", mail(hex(0xc6), cybID, "Brannoc", strings.Repeat("é", 65), "", 0, 1, 0), "23514", "character_mails_subject"},
		{"a body behind the byte limit", mail(hex(0xc7), cybID, "Brannoc", "Hello", strings.Repeat("b", 4097), 0, 1, 0), "23514", "character_mails_body"},
		{"a negative status", mail(hex(0xc8), cybID, "Brannoc", "Hello", "", -1, 1, 0), "23514", "character_mails_status"},
		{"an issue time of 0", mail(hex(0xc9), cybID, "Brannoc", "Hello", "", 0, 0, 0), "23514", "character_mails_issued"},
		{"a negative expiry", mail(hex(0xca), cybID, "Brannoc", "Hello", "", 0, 1, -1), "23514", "character_mails_expires"},
		{"a mail of a character that does not exist", mail(hex(0xcb), "ffffffffffffffffffffffffffffffff", "Brannoc", "Hello", "", 0, 1, 0), "23503", "character_mails_character_id_fkey"},
		{"a mail id twice", mail(mailA, cybID, "Brannoc", "Hello", "", 0, 1, 0), "23505", "character_mails_pkey"},

		{"an item of a mail that does not exist", item(hex(0xd1), hex(0x10), 0), "23503", "character_mail_items_mail_id_fkey"},
		{"a negative place", item(mailB, hex(0x11), -1), "23514", "character_mail_items_position"},
		{"the place after the last attachment", item(mailB, hex(0x12), 20), "23514", "character_mail_items_position"},
		{"an item record of 0", put(mailB, hex(0x13), 2, 0, 1, 1, 0, nil, nil, 0, false), "23514", "character_mail_items_record_positive"},
		{"an item guid in capitals", item(mailB, strings.ToUpper(hex(0xabc)), 2), "23514", "character_mail_items_guid_hex"},
		{"the zero item guid", item(mailB, strings.Repeat("0", 32), 2), "23514", "character_mail_items_guid_hex"},
		{"a quantity of 0", put(mailB, hex(0x15), 2, 7001, 0, 1, 0, nil, nil, 0, false), "23514", "character_mail_items_quantity"},
		{"a quantity past the limit", put(mailB, hex(0x16), 2, 7001, 1000001, 1, 0, nil, nil, 0, false), "23514", "character_mail_items_quantity"},
		{"a quality of 0", put(mailB, hex(0x17), 2, 7001, 1, 0, 0, nil, nil, 0, false), "23514", "character_mail_items_quality"},
		{"a quality past the limit", put(mailB, hex(0x18), 2, 7001, 1, 201, 0, nil, nil, 0, false), "23514", "character_mail_items_quality"},
		{"a negative durability", put(mailB, hex(0x19), 2, 7001, 1, 1, -1, nil, nil, 0, false), "23514", "character_mail_items_durability"},
		{"a durability past the limit", put(mailB, hex(0x1a), 2, 7001, 1, 1, 2e9, nil, nil, 0, false), "23514", "character_mail_items_durability"},
		{"a negative persisted health", put(mailB, hex(0x1b), 2, 7001, 1, 1, 0, ptrF(-1), nil, 0, false), "23514", "character_mail_items_persisted_health"},
		{"a persisted health past the limit", put(mailB, hex(0x1c), 2, 7001, 1, 1, 0, ptrF(2e9), nil, 0, false), "23514", "character_mail_items_persisted_health"},
		{"a negative persisted mana", put(mailB, hex(0x1d), 2, 7001, 1, 1, 0, nil, ptrF(-1), 0, false), "23514", "character_mail_items_persisted_mana"},
		{"a lock below 0", put(mailB, hex(0x1e), 2, 7001, 1, 1, 0, nil, nil, -1, false), "23514", "character_mail_items_lock_expiration"},
		{"a lock past the year 9999", put(mailB, hex(0x1f), 2, 7001, 1, 1, 0, nil, nil, 253402300800, false), "23514", "character_mail_items_lock_expiration"},
		{"a place twice on one mail", item(mailB, hex(0x20), 0), "23505", "character_mail_items_pkey"},
		{"an item guid twice on one mail", item(mailB, hex(1), 2), "23505", "character_mail_items_guid_unique"},
	} {
		expectConstraint(t, tc.name, tc.err, tc.code, tc.constraint)
	}
	var mails, items int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_mails WHERE character_id = $1`, cybID).Scan(&mails); err != nil || mails != 2 {
		t.Errorf("cyb keeps %d mails (%v), want the 2 that were accepted", mails, err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_mail_items WHERE mail_id = $1`, mailA).Scan(&items); err != nil || items != 3 {
		t.Errorf("mail A keeps %d items (%v), want the 3 that were accepted", items, err)
	}
	// A deleted mail row takes its items with it; the other mail keeps its own.
	if _, err := pool.Exec(ctx, `DELETE FROM character_mails WHERE id = $1`, mailB); err != nil {
		t.Fatal(err)
	}
	var left int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_mail_items WHERE mail_id = $1`, mailB).Scan(&left); err != nil || left != 0 {
		t.Errorf("a deleted mail left %d items (%v), want none", left, err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_mail_items WHERE mail_id = $1`, mailA).Scan(&left); err != nil || left != 3 {
		t.Errorf("the other mail holds %d items (%v), want its 3", left, err)
	}
}

func ptrF(v float64) *float64 { return &v }

// The mails go with their character: deleting it removes both tables' rows and no other character's.
func TestMailRowsAreRemovedWithTheirCharacter(t *testing.T) {
	ctx := context.Background()
	pool := before27(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	const other = "00000000000000000000000000000001"
	for n, id := range []string{cybID, other} {
		mailID := fmt.Sprintf("%032x", 0x5000+n)
		if _, err := pool.Exec(ctx, `INSERT INTO character_mails (id, character_id, sender, subject, body, status, issued_at_ms, expires_at_ms)
			VALUES ($1, $2, 'Brannoc', 'Welcome', '', 0, 1771000000000, 0)`, mailID, id); err != nil {
			t.Fatal(err)
		}
		for position := 0; position < 2; position++ {
			guid := fmt.Sprintf("%032x", 0x6000+n*10+position)
			if _, err := pool.Exec(ctx, `INSERT INTO character_mail_items (mail_id, position, item_record_id, instance_guid, quantity, quality, durability,
				persisted_health, persisted_mana, lock_expiration, claimed) VALUES ($1, $2, 7001, $3, 1, 1, 0, NULL, NULL, 0, false)`,
				mailID, position, guid); err != nil {
				t.Fatal(err)
			}
		}
	}
	if _, err := pool.Exec(ctx, `DELETE FROM characters WHERE id = $1`, cybID); err != nil {
		t.Fatal(err)
	}
	var mine, theirs int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_mails WHERE character_id = $1`, cybID).Scan(&mine); err != nil || mine != 0 {
		t.Errorf("a deleted character keeps %d mails (%v), want none", mine, err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_mail_items i JOIN character_mails m ON m.id = i.mail_id WHERE m.character_id = $1`, cybID).Scan(&mine); err != nil || mine != 0 {
		t.Errorf("a deleted character keeps %d attachments (%v), want none", mine, err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_mails WHERE character_id = $1`, other).Scan(&theirs); err != nil || theirs != 1 {
		t.Errorf("another character keeps %d mails (%v), want its 1", theirs, err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_mail_items i JOIN character_mails m ON m.id = i.mail_id WHERE m.character_id = $1`, other).Scan(&theirs); err != nil || theirs != 2 {
		t.Errorf("another character keeps %d attachments (%v), want its 2", theirs, err)
	}
}

// The rollback in services/README.md ("Rolling back migration 0027"), run as written: the two tables and the record of the version go, nothing else changes,
// and the migration applies again.
func TestMigration0027RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0027"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	rollback := block[:strings.Index(block, "\n```")]

	pool := before27(t)
	schemaBefore := schemaWithoutMails(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_mails (id, character_id, sender, subject, body, status, issued_at_ms, expires_at_ms)
		VALUES ('11112222333344445555666677778888', $1, 'Brannoc', 'Welcome', 'Body', 0, 1771000000000, 0)`, cybID); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_mail_items (mail_id, position, item_record_id, instance_guid, quantity, quality, durability,
		persisted_health, persisted_mana, lock_expiration, claimed) VALUES ('11112222333344445555666677778888', 0, 7001, '0123456789abcdef0123456789abcdef', 1, 1, 11, NULL, NULL, 0, false)`); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("the README's rollback: %v", err)
	}
	for _, tb := range mailTables {
		if tableExists(t, pool, tb) {
			t.Errorf("%s is still there after the rollback", tb)
		}
	}
	var recorded int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 27`).Scan(&recorded); err != nil || recorded != 0 {
		t.Errorf("version 27 is still recorded after the rollback (%d, %v)", recorded, err)
	}
	if after := schemaWithoutMails(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Error("the rollback left the schema other than it was before 0027")
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate again after the rollback: %v", err)
	}
	for _, tb := range mailTables {
		if !tableExists(t, pool, tb) {
			t.Errorf("%s was not made again", tb)
		}
	}
}
