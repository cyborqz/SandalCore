// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0024 (social, step 7: the friends list). It ADDS TWO TABLES, character_friends and character_friend_requests, and touches nothing else (every migration is add-only). It is
// tested for what it must not do first (every character reads back as it was, no other table, column, constraint or index changes), then for what it makes (the keys, the places, the
// cascade that removes a deleted character's lists), a second run changes nothing, the rollback is the README's block, run as written, and a fresh database and one at the migration before
// it both end with the two tables. The last two tests are about the number: 0020 stays free for the change that owns it, 0024 is recorded once, and a database that records 22 and 23 (another branch's) still gets the tables.

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

var friendTables = []string{"character_friends", "character_friend_requests"}

// before24 is the schema as it is at the migration before this one (0021, the ignore list), with the real-shaped characters.  upTo takes the versions below its limit, so 24 stops below
// this migration, at 0021 in this tree.
func before24(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool, _, _ := legacySetup(t)
	if err := migrate(context.Background(), pool, upTo(t, 24)); err != nil {
		t.Fatalf("migrate to 0021: %v", err)
	}
	return pool
}

// schemaWithoutFriends lists every column, constraint, index and table but the two of 0024: what 0024 must not change.
func schemaWithoutFriends(t *testing.T, pool *pgxpool.Pool) []string {
	t.Helper()
	const skip = `('character_friends', 'character_friend_requests')`
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

func TestMigration0024AddsTheFriendTablesAndTouchesNothingElse(t *testing.T) {
	ctx := context.Background()
	pool := before24(t)
	chars := rows(t, pool, true)
	if len(chars) == 0 {
		t.Fatal("the play database has no characters")
	}
	for _, tb := range friendTables {
		if tableExists(t, pool, tb) {
			t.Fatalf("%s exists before 0024", tb)
		}
	}
	schemaBefore := schemaWithoutFriends(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	for _, tb := range friendTables {
		if !tableExists(t, pool, tb) {
			t.Fatalf("0024 did not make %s", tb)
		}
		var n int
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM `+tb).Scan(&n); err != nil || n != 0 {
			t.Errorf("%s has %d rows after the migration (%v), want none: nothing is backfilled", tb, n, err)
		}
	}
	if after := schemaWithoutFriends(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Errorf("0024 changed more than its two tables:\nbefore %d entries, after %d", len(schemaBefore), len(after))
	}
	after := rows(t, pool, true)
	if len(after) != len(chars) {
		t.Fatalf("%d characters after, %d before", len(after), len(chars))
	}
	for id, b := range chars {
		a := after[id]
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil // pointers: compared by value
		if a != b {
			t.Errorf("%s changed by 0024", b.name)
		}
	}
	// the version is recorded once, and nothing is recorded at 20: that number stays free for the change that owns it
	versions := appliedVersions(t, pool)
	if !slices.Contains(versions, 24) || slices.Contains(versions, 20) {
		t.Errorf("applied versions = %v, want 24 in it and 20 not", versions)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Errorf("second migrate: %v", err)
	}
	if again := appliedVersions(t, pool); !slices.Equal(again, versions) {
		t.Errorf("the second migrate changed the applied versions: %v then %v", versions, again)
	}
}

// A fresh, empty database ends with both tables, the version recorded and the keys and the expiry index there.
func TestMigration0024FromAnEmptyDatabase(t *testing.T) {
	ctx := context.Background()
	pool := dbtest.Pool(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	for _, tb := range friendTables {
		if !tableExists(t, pool, tb) {
			t.Fatalf("a fresh database has no %s after Migrate", tb)
		}
	}
	if !slices.Contains(appliedVersions(t, pool), 24) {
		t.Errorf("a fresh database did not record version 24: %v", appliedVersions(t, pool))
	}
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM pg_indexes WHERE schemaname = current_schema() AND indexname = 'character_friend_requests_expiry'`).Scan(&n); err != nil || n != 1 {
		t.Errorf("the expiry index: %d, %v; want 1", n, err)
	}
}

func TestFriendTablesConstraints(t *testing.T) {
	ctx := context.Background()
	pool := before24(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	hex := func(n int) string { return fmt.Sprintf("%032x", n) }
	friend := func(character, guid, name string, level, class, sub int, guild string, still bool, position int) error {
		_, err := pool.Exec(ctx, `INSERT INTO character_friends (character_id, friend_guid, friend_name, level, class_id, sub_archetype, guild_name, still_friend, position)
			VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)`, character, guid, name, level, class, sub, guild, still, position)
		return err
	}
	request := func(character, sender, name, recipient string, expires int64, position int) error {
		_, err := pool.Exec(ctx, `INSERT INTO character_friend_requests (character_id, sender_guid, sender_name, recipient_guid, expires_at_ms, position)
			VALUES ($1, $2, $3, $4, $5, $6)`, character, sender, name, recipient, expires, position)
		return err
	}
	// what a list is: a friend with every field, a friend without a guild, one that no longer counts the character as a friend, the same friend of another character, and a request
	for _, tc := range []struct {
		name string
		err  error
	}{
		{"a friend with every field", friend(cybID, hex(1), "Brannoc", 25, 7, 2, "Sandal Makers", true, 0)},
		{"a friend without a guild", friend(cybID, hex(2), "Mirelle", 1, 0, 0, "", false, 1)},
		{"a friend outside ASCII", friend(cybID, hex(3), "Zoë", 12, 8, 8, "", true, 2)},
		{"the longest friend name", friend(cybID, hex(4), strings.Repeat("n", 64), 12, 8, 0, "", true, 3)},
		{"another character with the same friend", friend("00000000000000000000000000000002", hex(1), "Brannoc", 25, 7, 0, "", true, 0)},
		{"a request", request(cybID, hex(10), "Mirelle", cybID, 1771000000000, 0)},
		{"a second request", request(cybID, hex(11), "Tavish", cybID, 1771000000000, 1)},
		{"the same sender for another character", request("00000000000000000000000000000001", hex(10), "Mirelle", "00000000000000000000000000000001", 1771000000000, 0)},
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
		{"a friend guid in capitals", friend(cybID, strings.ToUpper(hex(0xabc)), "Brannoc2", 5, 2, 0, "", true, 10), "23514", "character_friends_guid_hex"},
		{"the zero friend guid", friend(cybID, strings.Repeat("0", 32), "Brannoc3", 5, 2, 0, "", true, 11), "23514", "character_friends_guid_hex"},
		{"an empty friend name", friend(cybID, hex(21), "", 5, 2, 0, "", true, 12), "23514", "character_friends_name"},
		{"a friend name with a space in front", friend(cybID, hex(22), " Brannoc", 5, 2, 0, "", true, 13), "23514", "character_friends_name"},
		{"a friend name behind the limit", friend(cybID, hex(23), strings.Repeat("n", 65), 5, 2, 0, "", true, 14), "23514", "character_friends_name"},
		{"a friend level 0", friend(cybID, hex(24), "Brannoc4", 0, 2, 0, "", true, 15), "23514", "character_friends_level"},
		{"a friend level 26", friend(cybID, hex(25), "Brannoc5", 26, 2, 0, "", true, 16), "23514", "character_friends_level"},
		{"a friend class 9", friend(cybID, hex(26), "Brannoc6", 5, 9, 0, "", true, 17), "23514", "character_friends_class"},
		{"a friend subclass 9", friend(cybID, hex(27), "Brannoc7", 5, 2, 9, "", true, 18), "23514", "character_friends_sub_archetype"},
		{"a guild name behind the limit", friend(cybID, hex(28), "Brannoc8", 5, 2, 0, strings.Repeat("g", 65), true, 19), "23514", "character_friends_guild"},
		{"a negative place", friend(cybID, hex(29), "Brannoc9", 5, 2, 0, "", true, -1), "23514", "character_friends_position"},
		{"the place after the last of a full list", friend(cybID, hex(30), "Brannoc10", 5, 2, 0, "", true, 100), "23514", "character_friends_position"},
		{"the character as its own friend", friend(cybID, cybID, "Brannoc11", 5, 2, 0, "", true, 20), "23514", "character_friends_not_self"},
		{"a friend guid twice", friend(cybID, hex(1), "Brannoc12", 5, 2, 0, "", true, 21), "23505", "character_friends_pkey"},
		{"a place twice", friend(cybID, hex(31), "Brannoc13", 5, 2, 0, "", true, 0), "23505", "character_friends_order"},
		{"a character that does not exist", friend("ffffffffffffffffffffffffffffffff", hex(32), "Brannoc14", 5, 2, 0, "", true, 0), "23503", "character_friends_character_id_fkey"},

		{"a request to the character itself", request(cybID, cybID, "Cyb", cybID, 1771000000000, 10), "23514", "character_friend_requests_not_self"},
		{"a request for another recipient", request(cybID, hex(40), "Mirelle2", "00000000000000000000000000000002", 1771000000000, 11), "23514", "character_friend_requests_recipient"},
		{"a sender guid in capitals", request(cybID, strings.ToUpper(hex(0xabc)), "Mirelle3", cybID, 1771000000000, 12), "23514", "character_friend_requests_sender_hex"},
		{"an empty sender name", request(cybID, hex(42), "", cybID, 1771000000000, 13), "23514", "character_friend_requests_name"},
		{"an expiry of 0", request(cybID, hex(43), "Mirelle4", cybID, 0, 14), "23514", "character_friend_requests_expires"},
		{"a negative expiry", request(cybID, hex(44), "Mirelle5", cybID, -1, 15), "23514", "character_friend_requests_expires"},
		{"a request place after the last of a full list", request(cybID, hex(45), "Mirelle6", cybID, 1771000000000, 100), "23514", "character_friend_requests_position"},
		{"a sender twice", request(cybID, hex(10), "Mirelle7", cybID, 1771000000000, 16), "23505", "character_friend_requests_pkey"},
		{"a request place twice", request(cybID, hex(46), "Mirelle8", cybID, 1771000000000, 0), "23505", "character_friend_requests_order"},
		{"a request of a character that does not exist", request("ffffffffffffffffffffffffffffffff", hex(47), "Mirelle9", "ffffffffffffffffffffffffffffffff", 1771000000000, 0), "23503", "character_friend_requests_character_id_fkey"},
	} {
		expectConstraint(t, tc.name, tc.err, tc.code, tc.constraint)
	}
	var friends, requests int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_friends WHERE character_id = $1`, cybID).Scan(&friends); err != nil || friends != 4 {
		t.Errorf("cyb keeps %d friends (%v), want the 4 that were accepted", friends, err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_friend_requests WHERE character_id = $1`, cybID).Scan(&requests); err != nil || requests != 2 {
		t.Errorf("cyb keeps %d requests (%v), want the 2 that were accepted", requests, err)
	}
}

// The lists go with their character: deleting it removes its rows and no other character's.
func TestFriendRowsAreRemovedWithTheirCharacter(t *testing.T) {
	ctx := context.Background()
	pool := before24(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	const other = "00000000000000000000000000000001"
	guid := "0123456789abcdef0123456789abcdef"
	for _, id := range []string{cybID, other} {
		if _, err := pool.Exec(ctx, `INSERT INTO character_friends (character_id, friend_guid, friend_name, level, class_id, sub_archetype, guild_name, still_friend, position)
			VALUES ($1, $2, 'Brannoc', 5, 2, 0, '', true, 0)`, id, guid); err != nil {
			t.Fatal(err)
		}
		if _, err := pool.Exec(ctx, `INSERT INTO character_friend_requests (character_id, sender_guid, sender_name, recipient_guid, expires_at_ms, position)
			VALUES ($1, $2, 'Mirelle', $1, 1771000000000, 0)`, id, guid); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := pool.Exec(ctx, `DELETE FROM characters WHERE id = $1`, cybID); err != nil {
		t.Fatal(err)
	}
	for _, tb := range friendTables {
		var mine, theirs int
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM `+tb+` WHERE character_id = $1`, cybID).Scan(&mine); err != nil || mine != 0 {
			t.Errorf("a deleted character keeps %d rows of %s (%v)", mine, tb, err)
		}
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM `+tb+` WHERE character_id = $1`, other).Scan(&theirs); err != nil || theirs != 1 {
			t.Errorf("another character keeps %d rows of %s (%v), want its 1", theirs, tb, err)
		}
	}
}

// The rollback in services/README.md ("Rolling back migration 0024"), run as written: the two tables and the record of the version go, nothing else changes, and the migration applies again.
func TestMigration0024RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0024"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	rollback := block[:strings.Index(block, "\n```")]

	pool := before24(t)
	schemaBefore := schemaWithoutFriends(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_friends (character_id, friend_guid, friend_name, level, class_id, sub_archetype, guild_name, still_friend, position)
		VALUES ($1, '0123456789abcdef0123456789abcdef', 'Brannoc', 5, 2, 0, '', true, 0)`, cybID); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("the README's rollback: %v", err)
	}
	for _, tb := range friendTables {
		if tableExists(t, pool, tb) {
			t.Errorf("%s is still there after the rollback", tb)
		}
	}
	var recorded int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 24`).Scan(&recorded); err != nil || recorded != 0 {
		t.Errorf("version 24 is still recorded after the rollback (%d, %v)", recorded, err)
	}
	if after := schemaWithoutFriends(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Error("the rollback left the schema other than it was before 0024")
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate again after the rollback: %v", err)
	}
	for _, tb := range friendTables {
		if !tableExists(t, pool, tb) {
			t.Errorf("%s was not made again", tb)
		}
	}
}

// The number again, from the real database. A branch that is still in progress numbers its two migrations 0022 and 0023, and a database that branch has run
// records both versions. The runner skips a version it has recorded, so a friends migration numbered 22 or 23 is never run there: the character service then fails at every login with
// "relation character_friend_requests does not exist". A database that records 22 and 23 and has none of the friend tables must get the tables when the migrations run.
func TestFriendTablesAreMadeOnADatabaseThatAlreadyRecordsVersions22And23(t *testing.T) {
	ctx := context.Background()
	pool := dbtest.Pool(t)
	if err := migrate(ctx, pool, upTo(t, 22)); err != nil { // 1 to 19 and 21: what a database holds before that branch's two
		t.Fatalf("migrate to 0021: %v", err)
	}
	for _, version := range []int{22, 23} {
		if _, err := pool.Exec(ctx, `INSERT INTO schema_migrations (version) VALUES ($1)`, version); err != nil {
			t.Fatal(err)
		}
	}
	for _, tb := range friendTables {
		if tableExists(t, pool, tb) {
			t.Fatalf("%s exists before the migrations ran", tb)
		}
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	for _, tb := range friendTables {
		if !tableExists(t, pool, tb) {
			t.Errorf("%s was not made on a database that records versions 22 and 23", tb)
		}
	}
	// the friends migration is recorded under a number above 23, and 22 and 23 are recorded once each, as they were
	entries, err := migrations.ReadDir("migrations")
	if err != nil {
		t.Fatal(err)
	}
	friendsVersion := 0
	for _, e := range entries {
		if m := migrationName.FindStringSubmatch(e.Name()); m != nil && strings.HasSuffix(e.Name(), "_character_friends.sql") {
			fmt.Sscanf(m[1], "%d", &friendsVersion)
		}
	}
	if friendsVersion <= 23 {
		t.Errorf("the friends migration is numbered %d, want above 23 (22 and 23 belong to the branch in progress)", friendsVersion)
	}
	applied := appliedVersions(t, pool)
	if !slices.Contains(applied, friendsVersion) {
		t.Errorf("applied versions = %v, want %d (the friends migration) in it", applied, friendsVersion)
	}
	for _, version := range []int{22, 23} {
		if !slices.Contains(applied, version) {
			t.Errorf("version %d is no longer recorded: %v", version, applied)
		}
	}
}
