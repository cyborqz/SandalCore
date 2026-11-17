// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
package db

import (
	"context"
	"os"
	"regexp"
	"testing"

	"aocrop/services/internal/dbtest"
)

const guildIdA = "11112222333344445555666677778888"
const guildIdB = "88887777666655554444333322221111"
const guildMemberId = "0123456789abcdef0123456789abcdef"

// Migration 0026 (guilds) adds four tables with the store's rules; the rollback block of services/README.md takes them away and forgets the version.
func TestMigration0026RollbackFromTheREADME(t *testing.T) {
	ctx := context.Background()
	pool := dbtest.Pool(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	tables := []string{"guilds", "guild_ranks", "guild_members", "character_guild_cooldowns"}
	has := func(name string) bool {
		var yes bool
		if err := pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = $1)`, name).Scan(&yes); err != nil {
			t.Fatalf("asking for %s: %v", name, err)
		}
		return yes
	}
	for _, name := range tables {
		if !has(name) {
			t.Fatalf("%s does not exist after the migration", name)
		}
	}

	// A guild needs a name, and no two guilds may hold the same name in another case.
	if _, err := pool.Exec(ctx, `INSERT INTO guilds (id, name) VALUES ($1, 'Sandal Makers')`, guildIdA); err != nil {
		t.Fatalf("a guild: %v", err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO guilds (id, name) VALUES ($1, 'sandal makers')`, guildIdB); err == nil {
		t.Fatal("a second guild took the same name in another case")
	}
	if _, err := pool.Exec(ctx, `INSERT INTO guilds (id, name) VALUES ('00000000000000000000000000000000', 'Nobody')`); err == nil {
		t.Fatal("a guild took the all-zero id")
	}
	// The permission mask is below 2^35.
	if _, err := pool.Exec(ctx, `INSERT INTO guild_ranks (guild_id, rank_index, rank_name, permissions) VALUES ($1, 0, 'Guild Leader', 34359738368)`, guildIdA); err == nil {
		t.Fatal("a rank took a 36th permission")
	}
	if _, err := pool.Exec(ctx, `INSERT INTO guild_ranks (guild_id, rank_index, rank_name, permissions) VALUES ($1, 0, 'Guild Leader', 34359738367)`, guildIdA); err != nil {
		t.Fatalf("a rank with all 35 permissions: %v", err)
	}
	// A character is in at most one guild.
	if _, err := pool.Exec(ctx, `INSERT INTO accounts (id, username, password_hash) VALUES (1, 'guildtest', 'x')`); err != nil {
		t.Fatalf("an account: %v", err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO characters (id, account_id, name, race_id, gender_id, archetype_id, class_name) VALUES ($1, 1, 'Cyb', 0, 0, 17747, 'Bard')`, guildMemberId); err != nil {
		t.Fatalf("a character: %v", err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO guild_members (guild_id, character_id, rank_index, joined_ms) VALUES ($1, $2, 0, 1)`, guildIdA, guildMemberId); err != nil {
		t.Fatalf("a member: %v", err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO guild_members (guild_id, character_id, rank_index, joined_ms) VALUES ($1, $2, 0, 1)`, guildIdB, guildMemberId); err == nil {
		t.Fatal("a character joined a second guild")
	}
	// Deleting the character removes the membership and leaves the guild.
	if _, err := pool.Exec(ctx, `DELETE FROM characters WHERE id = $1`, guildMemberId); err != nil {
		t.Fatalf("deleting the character: %v", err)
	}
	var members int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM guild_members WHERE guild_id = $1`, guildIdA).Scan(&members); err != nil {
		t.Fatal(err)
	}
	if members != 0 {
		t.Fatalf("the membership stayed after the character was deleted (%d rows)", members)
	}
	if _, err := pool.Exec(ctx, `DELETE FROM guilds WHERE id = $1`, guildIdA); err != nil {
		t.Fatalf("deleting the guild: %v", err)
	}

	readme, err := os.ReadFile("../../README.md")
	if err != nil {
		t.Fatal(err)
	}
	block := regexp.MustCompile(`(?s)-- rollback of migration 0026[^\n]*\n(BEGIN;.*?COMMIT;)`).FindSubmatch(readme)
	if block == nil {
		t.Fatal("services/README.md has no rollback block for 0026")
	}
	if _, err := pool.Exec(ctx, string(block[1])); err != nil {
		t.Fatalf("the rollback: %v", err)
	}
	for _, name := range tables {
		if has(name) {
			t.Fatalf("the rollback left %s", name)
		}
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatalf("migrating again after the rollback: %v", err)
	}
	for _, name := range tables {
		if !has(name) {
			t.Fatalf("%s did not come back", name)
		}
	}
}
