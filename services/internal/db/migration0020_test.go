// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
package db

import (
	"context"
	"os"
	"regexp"
	"testing"

	"aocrop/services/internal/dbtest"
)

// Migration 0020 (node citizenship) adds one table; the rollback block of services/README.md takes it away and forgets its version.
func TestMigration0020RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	pool := dbtest.Pool(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	has := func() bool {
		var yes bool
		if err := pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = 'account_citizenship')`).Scan(&yes); err != nil {
			t.Fatal(err)
		}
		return yes
	}
	if !has() {
		t.Fatal("the table does not exist after the migration")
	}
	readme, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	block := regexp.MustCompile("(?s)-- rollback of migration 0020[^\n]*\n(BEGIN;.*?COMMIT;)").FindSubmatch(readme)
	if block == nil {
		t.Fatal("services/README.md has no rollback block for 0020")
	}
	if _, err := pool.Exec(ctx, string(block[1])); err != nil {
		t.Fatalf("the rollback: %v", err)
	}
	if has() {
		t.Fatal("the rollback left the table")
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrating again after the rollback: %v", err)
	}
	if !has() {
		t.Fatal("the table did not come back")
	}
}
