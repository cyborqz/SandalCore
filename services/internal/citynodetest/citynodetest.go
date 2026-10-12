// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package citynodetest builds a started city node service on a throwaway schema, for the tests of citynode, citynodegrpc and
// nodectl. Like dbtest it skips the test when AOC_TEST_DATABASE_URL is unset.
package citynodetest

import (
	"context"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"

	"aocrop/services/internal/citynode"
	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"
)

// SeedJoevaHalcyon is deploy/citynode_seed.json's content in short: Joeva a Town, Halcyon a Village and Joeva's vassal.
const SeedJoevaHalcyon = `{"nodes": {"Verra_RVR_Joeva": {"level": 4}, "Verra_RVR_Halcyon": {"level": 3, "parent": "Verra_RVR_Joeva"}}}`

// Clock is a real time the test moves by hand.
type Clock struct {
	mu sync.Mutex
	t  time.Time
}

// NewClock starts at a fixed instant.
func NewClock() *Clock { return &Clock{t: time.Date(2026, 10, 9, 12, 0, 0, 0, time.UTC)} }

// Now is the clock's time.
func (c *Clock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.t
}

// Advance moves it.
func (c *Clock) Advance(d time.Duration) {
	c.mu.Lock()
	c.t = c.t.Add(d)
	c.mu.Unlock()
}

// Env is a migrated schema and a service over it.
type Env struct {
	Pool     *pgxpool.Pool
	Svc      *citynode.Service
	Clock    *Clock
	SeedPath string
}

// -- Cyb x C | O | A --

// New migrates a fresh schema, writes seed to a file and makes a service over it with a hand-moved real time. It does not
// start the service (Env.Start does), so a test can look at the tables first.
func New(t *testing.T, seed string) *Env {
	t.Helper()
	pool := dbtest.Pool(t)
	if err := db.Migrate(context.Background(), pool); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	path := filepath.Join(t.TempDir(), "citynode_seed.json")
	if seed != "" {
		if err := os.WriteFile(path, []byte(seed), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	clk := NewClock()
	return &Env{Pool: pool, Clock: clk, SeedPath: path,
		Svc: citynode.New(pool, citynode.DefaultRoster(), path, citynode.WithClock(clk.Now))}
}

// Start starts the service (the node rows, the seed).
func (e *Env) Start(t *testing.T) {
	t.Helper()
	if err := e.Svc.Start(context.Background()); err != nil {
		t.Fatalf("start: %v", err)
	}
}

// Another is a second service over the same schema and seed file (a restart of citynode).
func (e *Env) Another() *citynode.Service {
	return citynode.New(e.Pool, citynode.DefaultRoster(), e.SeedPath, citynode.WithClock(e.Clock.Now))
}
