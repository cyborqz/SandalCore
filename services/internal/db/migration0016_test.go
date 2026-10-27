// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0016 (quests, slice 1, "take a quest"): the quest service's four tables. It ADDS FOUR TABLES and touches nothing else (every migration is add-only). It is tested for what it
// must not do first (every character reads back as it was, no other table or column changes), then for what it makes (the constraints, the cascade that removes a deleted character's
// quests), a second run changes nothing, and the rollback is the README's block, run as written.

import (
	"context"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5/pgxpool"
)

var questTables = []string{"character_quests", "character_quests_completed", "character_quest_variables", "character_quest_commands"}

// before16 is the schema as it is after 0015, with the real-shaped characters.
func before16(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool, _, _ := legacySetup(t)
	if err := migrate(context.Background(), pool, upTo(t, 16)); err != nil {
		t.Fatalf("migrate to 0015: %v", err)
	}
	return pool
}

// schemaWithoutQuests lists every column, constraint and table but the four of 0016: what 0016 must not change.
func schemaWithoutQuests(t *testing.T, pool *pgxpool.Pool) []string {
	t.Helper()
	const skip = `'character_quests', 'character_quests_completed', 'character_quest_variables', 'character_quest_commands'`
	var out []string
	for _, q := range []string{
		`SELECT table_name || '.' || column_name || ' ' || data_type || ' ' || is_nullable || ' ' || coalesce(column_default, '-') FROM information_schema.columns
			WHERE table_schema = current_schema() AND table_name NOT IN (` + skip + `) ORDER BY 1`,
		`SELECT conrelid::regclass::text || ' ' || conname || ' ' || pg_get_constraintdef(oid) FROM pg_constraint
			WHERE connamespace = current_schema()::regnamespace AND conrelid::regclass::text NOT IN (` + skip + `) ORDER BY 1`,
		`SELECT 'table ' || table_name FROM information_schema.tables WHERE table_schema = current_schema() AND table_name NOT IN (` + skip + `) ORDER BY 1`,
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

func TestMigration0016AddsFourTablesAndTouchesNothingElse(t *testing.T) {
	ctx := context.Background()
	pool := before16(t)
	chars := rows(t, pool, true)
	if len(chars) == 0 {
		t.Fatal("the play database has no characters")
	}
	for _, tb := range questTables {
		if tableExists(t, pool, tb) {
			t.Fatalf("%s exists before 0016", tb)
		}
	}
	schemaBefore := schemaWithoutQuests(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	for _, tb := range questTables {
		if !tableExists(t, pool, tb) {
			t.Fatalf("0016 did not make %s", tb)
		}
		var n int
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM `+tb).Scan(&n); err != nil || n != 0 {
			t.Errorf("%s has %d rows after the migration (%v), want none", tb, n, err)
		}
	}
	if after := schemaWithoutQuests(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Errorf("0016 changed more than its four tables:\nbefore %d entries, after %d", len(schemaBefore), len(after))
	}
	after := rows(t, pool, true)
	if len(after) != len(chars) {
		t.Fatalf("%d characters after, %d before", len(after), len(chars))
	}
	for id, b := range chars {
		a := after[id]
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil // pointers: compared by value
		if a != b {
			t.Errorf("%s changed by 0016", b.name)
		}
	}

	// rows, and the constraints
	const guid = "0123456789abcdef0123456789abcdef"
	quest := func(g string, record int64, status int, position int, objectives string) error {
		_, err := pool.Exec(ctx, `INSERT INTO character_quests (character_id, instance_guid, record_id, status, position, objectives) VALUES ($1, $2, $3, $4, $5, $6::jsonb)`,
			cybID, g, record, status, position, objectives)
		return err
	}
	if err := quest(guid, 6064634563262021659, 0, 0, `[{"guid":"fedcba9876543210fedcba9876543210","record":6064634563268640796,"active":true,"awake":true}]`); err != nil {
		t.Fatal(err)
	}
	for _, bad := range []struct {
		name  string
		err   error
		field string
	}{
		{"the guid is taken", quest(guid, 7, 0, 1, `[]`), "character_quests_pkey"},
		{"the quest is in the log already", quest("ffffffffffffffffffffffffffffff01", 6064634563262021659, 0, 1, `[]`), "character_quests_record_unique"},
		{"the place is taken", quest("ffffffffffffffffffffffffffffff02", 8, 0, 0, `[]`), "character_quests_order"},
		{"a guid in capitals", quest(strings.ToUpper("ffffffffffffffffffffffffffffff03"), 9, 0, 1, `[]`), "character_quests_guid_hex"},
		{"the zero guid", quest(strings.Repeat("0", 32), 10, 0, 1, `[]`), "character_quests_guid_hex"},
		{"record 0", quest("ffffffffffffffffffffffffffffff04", 0, 0, 1, `[]`), "character_quests_record_positive"},
		{"status 4", quest("ffffffffffffffffffffffffffffff05", 11, 4, 1, `[]`), "character_quests_status"},
		{"a negative place", quest("ffffffffffffffffffffffffffffff06", 12, 0, -1, `[]`), "character_quests_position"},
		{"objectives that are not an array", quest("ffffffffffffffffffffffffffffff07", 13, 0, 1, `{}`), "character_quests_objectives"},
	} {
		if bad.err == nil || !strings.Contains(bad.err.Error(), bad.field) {
			t.Errorf("%s: %v, want a violation of %s", bad.name, bad.err, bad.field)
		}
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_quests (character_id, instance_guid, record_id, position) VALUES ('no such character', 'ffffffffffffffffffffffffffffff08', 14, 0)`); err == nil {
		t.Error("a quest of a character that does not exist was accepted")
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_quests_completed (character_id, record_id, completions) VALUES ($1, 6064634563262021659, 2)`, cybID); err != nil {
		t.Error(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_quests_completed (character_id, record_id, completions) VALUES ($1, 6064634563262021660, 0)`, cybID); err == nil || !strings.Contains(err.Error(), "character_quests_completed_count") {
		t.Errorf("zero completions: %v", err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_quest_variables (character_id, name, value) VALUES ($1, 'PickedSamias', 1)`, cybID); err != nil {
		t.Error(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_quest_variables (character_id, name, value) VALUES ($1, '', 1)`, cybID); err == nil || !strings.Contains(err.Error(), "character_quest_variables_name") {
		t.Errorf("an empty variable name: %v", err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_quest_commands (character_id, kind, quest) VALUES ($1, 'give', 'RVR_FTUE_QST_1415_OfTheRootways')`, cybID); err != nil {
		t.Error(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_quest_commands (character_id, kind) VALUES ($1, 'teleport')`, cybID); err == nil || !strings.Contains(err.Error(), "character_quest_commands_kind") {
		t.Errorf("an unknown command kind: %v", err)
	}

	// a second run changes nothing
	if err := Migrate(ctx, pool); err != nil {
		t.Errorf("second migrate: %v", err)
	}
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM character_quests WHERE character_id = $1`, cybID).Scan(&n); err != nil || n != 1 {
		t.Errorf("after the second migrate the character has %d quest rows (%v), want 1", n, err)
	}

	// deleting a character takes all its quests along
	if _, err := pool.Exec(ctx, `DELETE FROM characters WHERE id = $1`, cybID); err != nil {
		t.Fatal(err)
	}
	for _, tb := range questTables {
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM `+tb+` WHERE character_id = $1`, cybID).Scan(&n); err != nil || n != 0 {
			t.Errorf("%d rows of %s outlive their character (%v)", n, tb, err)
		}
	}
}

// The rollback in services/README.md ("Rolling back migration 0016"), run as written: the four tables and the record of the version go, everything else is as it was, and the migration
// applies again.
func TestMigration0016RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0016"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	rollback := block[:strings.Index(block, "\n```")]

	pool := before16(t)
	chars := rows(t, pool, true)
	schemaBefore := schemaWithoutQuests(t, pool)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO character_quests (character_id, instance_guid, record_id, position) VALUES ($1, 'eeeeeeeeeeeeeeeeeeeeeeeeeeeeee01', 7, 0)`, cybID); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("the README's rollback: %v", err)
	}
	for _, tb := range questTables {
		if tableExists(t, pool, tb) {
			t.Errorf("%s is still there after the rollback", tb)
		}
	}
	var recorded int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 16`).Scan(&recorded); err != nil || recorded != 0 {
		t.Errorf("version 16 is still recorded after the rollback (%d, %v)", recorded, err)
	}
	if after := schemaWithoutQuests(t, pool); strings.Join(after, "\n") != strings.Join(schemaBefore, "\n") {
		t.Error("the rollback left the schema other than it was before 0016")
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
	for _, tb := range questTables {
		if !tableExists(t, pool, tb) {
			t.Errorf("%s was not made again", tb)
		}
	}
}
