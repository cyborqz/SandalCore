// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package dbtest gives each integration test its own schema in the database
// named by AOC_TEST_DATABASE_URL, and skips the test when that is unset.
package dbtest

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"os"
	"testing"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

// ~ C | O | A & C-Y-B ~

// Pool returns a pool whose connections use a fresh, empty schema (with public
// still on the search path, so extensions installed there stay usable); the
// schema is dropped when the test ends. Every connection carries the schema
// name as its application_name, so a test can tell its own sessions apart from
// other tests running against the same database.
func Pool(t *testing.T) *pgxpool.Pool {
	t.Helper()
	url := os.Getenv("AOC_TEST_DATABASE_URL")
	if url == "" {
		t.Skip("AOC_TEST_DATABASE_URL not set (scripts/services.ps1 test sets it)")
	}
	ctx := context.Background()
	var b [6]byte
	if _, err := rand.Read(b[:]); err != nil {
		t.Fatal(err)
	}
	schema := "t_" + hex.EncodeToString(b[:])
	quoted := pgx.Identifier{schema}.Sanitize()

	admin, err := pgxpool.New(ctx, url)
	if err != nil {
		t.Fatalf("connect: %v", err)
	}
	if _, err := admin.Exec(ctx, "CREATE SCHEMA "+quoted); err != nil {
		admin.Close()
		t.Fatalf("create schema: %v", err)
	}
	// Registered before anything else can fail, so a later t.Fatal still
	// cleans up: close the test pool, drop the schema, then close admin.
	var pool *pgxpool.Pool
	t.Cleanup(func() {
		if pool != nil {
			pool.Close()
		}
		if _, err := admin.Exec(context.Background(), "DROP SCHEMA "+quoted+" CASCADE"); err != nil {
			t.Logf("drop schema %s: %v", schema, err)
		}
		admin.Close()
	})

	cfg, err := pgxpool.ParseConfig(url)
	if err != nil {
		t.Fatal(err)
	}
	cfg.ConnConfig.RuntimeParams["search_path"] = schema + ",public"
	cfg.ConnConfig.RuntimeParams["application_name"] = schema
	pool, err = pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		t.Fatal(err)
	}
	return pool
}
