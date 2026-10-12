// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package db

// Migration 0009 (Nodes 1, stage 1): the city node service's three tables. It ADDS TABLES ONLY, so it is tested for what it must not do first
// (every character reads back exactly as it was), then for what it makes (three tables; city_node_state empty, one clock row at real time, one
// seed row not yet applied; the constraints), a second run changes nothing, and the rollback is the README's block, run as written.

import (
	"context"
	"os"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5/pgxpool"
)

// before9 is the schema as it is after 0008 (the play database of the mounts stage), with the real-shaped characters.
func before9(t *testing.T) *pgxpool.Pool {
	t.Helper()
	pool, _, _ := legacySetup(t)
	if err := migrate(context.Background(), pool, upTo(t, 9)); err != nil {
		t.Fatalf("migrate to 0008: %v", err)
	}
	return pool
}

func cityNodeTables(t *testing.T, pool *pgxpool.Pool) []string {
	t.Helper()
	r, err := pool.Query(context.Background(), `SELECT table_name FROM information_schema.tables WHERE table_schema = current_schema() AND table_name LIKE 'city_node_%' ORDER BY table_name`)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	var out []string
	for r.Next() {
		var n string
		if err := r.Scan(&n); err != nil {
			t.Fatal(err)
		}
		out = append(out, n)
	}
	return out
}

// (C | Y | B ~ CoA)

func TestMigration0009AddsThreeTablesAndTouchesNothingElse(t *testing.T) {
	ctx := context.Background()
	pool := before9(t)
	chars := rows(t, pool, true)
	if len(chars) == 0 {
		t.Fatal("the play database has no characters")
	}
	if got := cityNodeTables(t, pool); len(got) != 0 {
		t.Fatalf("city node tables before 0009: %v", got)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	if got, want := strings.Join(cityNodeTables(t, pool), ","), "city_node_clock,city_node_seed,city_node_state"; got != want {
		t.Fatalf("tables after 0009 = %s, want %s", got, want)
	}
	after := rows(t, pool, true)
	if len(after) != len(chars) {
		t.Fatalf("%d characters after, %d before", len(after), len(chars))
	}
	for id, b := range chars {
		a := after[id]
		a.race, a.gender, a.class, b.race, b.gender, b.class = nil, nil, nil, nil, nil, nil // pointers: compared by value
		if a != b {
			t.Errorf("%s changed by 0009", b.name)
		}
	}

	// the node state starts empty (citynode makes the rows), the clock is real time, the seed is not applied
	var n int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM city_node_state`).Scan(&n); err != nil || n != 0 {
		t.Errorf("city_node_state has %d rows after the migration (%v), want none", n, err)
	}
	var speed float64
	var offset int64
	if err := pool.QueryRow(ctx, `SELECT speed, offset_seconds FROM city_node_clock`).Scan(&speed, &offset); err != nil || speed != 1 || offset != 0 {
		t.Errorf("the clock = speed %v offset %v (%v), want 1 and 0", speed, offset, err)
	}
	var applied *string
	if err := pool.QueryRow(ctx, `SELECT applied_at::text FROM city_node_seed`).Scan(&applied); err != nil || applied != nil {
		t.Errorf("the seed row = %v (%v), want applied_at NULL", applied, err)
	}

	// the constraints
	for _, bad := range []string{
		`INSERT INTO city_node_state (node_guid, level, updated_at) VALUES (1, -1, now())`,                 // level below 0
		`INSERT INTO city_node_state (node_guid, level, experience, updated_at) VALUES (1, 0, -5, now())`,  // XP below 0
		`INSERT INTO city_node_state (node_guid, level, parent_guid, updated_at) VALUES (1, 0, 1, now())`,  // its own lord
		`INSERT INTO city_node_state (node_guid, level, parent_guid, updated_at) VALUES (1, 0, 99, now())`, // a lord that is not a node
		`INSERT INTO city_node_state (node_guid, level) VALUES (1, 0)`,                                     // no updated_at
		`INSERT INTO city_node_clock (id, speed) VALUES (FALSE, 1)`,                                        // only one clock row
		`INSERT INTO city_node_seed (id) VALUES (FALSE)`,                                                   // only one seed row
		`UPDATE city_node_clock SET speed = 0`,                                                             // a stopped clock
	} {
		if _, err := pool.Exec(ctx, bad); err == nil {
			t.Errorf("accepted: %s", bad)
		}
	}
	if _, err := pool.Exec(ctx, `INSERT INTO city_node_state (node_guid, level, updated_at) VALUES (1, 4, now()), (2, 3, now())`); err != nil {
		t.Fatalf("a node row: %v", err)
	}
	if _, err := pool.Exec(ctx, `UPDATE city_node_state SET parent_guid = 1 WHERE node_guid = 2`); err != nil {
		t.Fatalf("a vassal link: %v", err)
	}

	// a second run changes nothing
	if err := Migrate(ctx, pool); err != nil {
		t.Errorf("second migrate: %v", err)
	}
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM city_node_state`).Scan(&n); err != nil || n != 2 {
		t.Errorf("after the second migrate city_node_state has %d rows (%v), want the 2", n, err)
	}
}

// The rollback in services/README.md ("Rolling back migration 0009"), run as written: the three tables and the record of the version go, the
// characters are as they were, and the migration applies again.
func TestMigration0009RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	raw, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	readme := strings.ReplaceAll(string(raw), "\r\n", "\n")
	const open = "```sql\n-- rollback of migration 0009"
	start := strings.Index(readme, open)
	if start < 0 {
		t.Fatalf("services/README.md has no %q block", open)
	}
	block := readme[start+len("```sql\n"):]
	rollback := block[:strings.Index(block, "\n```")]

	pool := before9(t)
	chars := rows(t, pool, true)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO city_node_state (node_guid, level, updated_at) VALUES (1, 4, now()), (2, 3, now())`); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `UPDATE city_node_state SET parent_guid = 1 WHERE node_guid = 2`); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, rollback); err != nil {
		t.Fatalf("the README's rollback: %v", err)
	}
	if got := cityNodeTables(t, pool); len(got) != 0 {
		t.Errorf("city node tables after the rollback: %v", got)
	}
	var recorded int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM schema_migrations WHERE version = 9`).Scan(&recorded); err != nil || recorded != 0 {
		t.Errorf("version 9 is still recorded after the rollback (%d, %v)", recorded, err)
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
	if got := cityNodeTables(t, pool); len(got) != 3 {
		t.Errorf("city node tables after migrating again: %v", got)
	}
}
