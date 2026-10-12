// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package mesh

import (
	"context"
	"errors"
	"math"
	"testing"
	"time"

	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"
)

// setup returns a store on a fresh schema whose clock the test moves.
func setup(t *testing.T, static string) (*Store, *time.Time) {
	t.Helper()
	pool := dbtest.Pool(t)
	if err := db.Migrate(context.Background(), pool); err != nil {
		t.Fatal(err)
	}
	s := NewStore(pool, 15*time.Second, static)
	now := time.Date(2026, 9, 29, 12, 0, 0, 0, time.UTC)
	s.now = func() time.Time { return now }
	return s, &now
}

func TestRegisterGivesDistinctIDsFromOne(t *testing.T) {
	s, _ := setup(t, "")
	ctx := context.Background()
	a, err := s.Register(ctx, "127.0.0.1:7777", []string{"verra"})
	if err != nil {
		t.Fatal(err)
	}
	b, err := s.Register(ctx, "127.0.0.1:7778", nil)
	if err != nil {
		t.Fatal(err)
	}
	if a < 1 || b < 1 || a == b {
		t.Errorf("server ids %d and %d, want distinct and >= 1", a, b)
	}
}

func TestRegisterRejectsBadInput(t *testing.T) {
	s, _ := setup(t, "")
	for _, tc := range []struct {
		address string
		regions []string
	}{
		{"", nil}, {"no-port", nil}, {"a\x00b:7777", nil}, {"\xff:7777", nil},
		{"127.0.0.1:7777", []string{"ok", "bad\x00region"}}, {"127.0.0.1:7777", []string{"bad\xffregion"}},
	} {
		if _, err := s.Register(context.Background(), tc.address, tc.regions); !errors.Is(err, ErrInvalid) {
			t.Errorf("Register(%q, %q) = %v, want ErrInvalid", tc.address, tc.regions, err)
		}
	}
}

func TestHeartbeatUnknownServer(t *testing.T) {
	s, _ := setup(t, "")
	for _, id := range []uint32{0, 999, math.MaxInt32 + 1, math.MaxUint32} {
		if err := s.Heartbeat(context.Background(), id); !errors.Is(err, ErrUnknownServer) {
			t.Errorf("Heartbeat(%d) = %v, want ErrUnknownServer", id, err)
		}
	}
}

// [Cyb x C O A]

func TestPlaceWithNoServerAndNoStaticWorld(t *testing.T) {
	s, _ := setup(t, "")
	if _, _, err := s.Place(context.Background(), "0123456789abcdef0123456789abcdef"); !errors.Is(err, ErrNoServer) {
		t.Errorf("Place = %v, want ErrNoServer", err)
	}
}

func TestPlaceFallsBackToTheStaticWorld(t *testing.T) {
	s, _ := setup(t, "127.0.0.1:7777")
	id, addr, err := s.Place(context.Background(), "0123456789abcdef0123456789abcdef")
	if err != nil || id != StaticServerID || addr != "127.0.0.1:7777" {
		t.Errorf("Place = %d %q %v, want the static world", id, addr, err)
	}
}

func TestPlacePrefersALiveRegisteredServer(t *testing.T) {
	s, _ := setup(t, "127.0.0.1:7777")
	ctx := context.Background()
	reg, err := s.Register(ctx, "192.0.2.5:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	id, addr, err := s.Place(ctx, "0123456789abcdef0123456789abcdef")
	if err != nil || id != reg || addr != "192.0.2.5:7001" {
		t.Errorf("Place = %d %q %v, want server %d", id, addr, err, reg)
	}
}

// A server that misses heartbeats for the TTL is not placed; a heartbeat
// brings it back.
func TestPlaceSkipsASilentServerUntilItHeartbeats(t *testing.T) {
	s, now := setup(t, "127.0.0.1:7777")
	ctx := context.Background()
	reg, err := s.Register(ctx, "192.0.2.5:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	*now = now.Add(16 * time.Second)
	if id, _, err := s.Place(ctx, "0123456789abcdef0123456789abcdef"); err != nil || id != StaticServerID {
		t.Errorf("after 16 s of silence: Place = %d %v, want the static world", id, err)
	}
	if err := s.Heartbeat(ctx, reg); err != nil {
		t.Fatal(err)
	}
	if id, _, err := s.Place(ctx, "0123456789abcdef0123456789abcdef"); err != nil || id != reg {
		t.Errorf("after a heartbeat: Place = %d %v, want %d", id, err, reg)
	}
}

func TestPlacePicksTheMostRecentHeartbeat(t *testing.T) {
	s, now := setup(t, "")
	ctx := context.Background()
	a, err := s.Register(ctx, "192.0.2.5:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	b, err := s.Register(ctx, "192.0.2.6:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	*now = now.Add(5 * time.Second)
	if err := s.Heartbeat(ctx, a); err != nil {
		t.Fatal(err)
	}
	if id, _, err := s.Place(ctx, "0123456789abcdef0123456789abcdef"); err != nil || id != a {
		t.Errorf("Place = %d %v, want %d (heartbeat after %d's)", id, err, a, b)
	}
}

const someChar = "0123456789abcdef0123456789abcdef"

// The TTL boundary: silent for less than the TTL is placed, for exactly the
// TTL is not ("silent this long is not placed").
func TestPlaceTTLBoundary(t *testing.T) {
	s, now := setup(t, "127.0.0.1:7777") // ttl is 15 s
	ctx := context.Background()
	reg, err := s.Register(ctx, "192.0.2.5:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	t0 := *now
	for _, tc := range []struct {
		silent time.Duration
		placed bool
	}{
		{14 * time.Second, true},
		{15*time.Second - time.Microsecond, true},
		{15 * time.Second, false},
		{15*time.Second + time.Microsecond, false},
	} {
		*now = t0.Add(tc.silent)
		id, _, err := s.Place(ctx, someChar)
		if err != nil || (id == reg) != tc.placed {
			t.Errorf("silent for %v: Place = %d %v, want placed=%v", tc.silent, id, err, tc.placed)
		}
	}
}

// Servers with the same last heartbeat: the one registered later wins, so the
// answer does not depend on row order.
func TestPlaceTieGoesToTheNewerRegistration(t *testing.T) {
	s, _ := setup(t, "")
	ctx := context.Background()
	if _, err := s.Register(ctx, "192.0.2.5:7001", nil); err != nil {
		t.Fatal(err)
	}
	b, err := s.Register(ctx, "192.0.2.6:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	if id, _, err := s.Place(ctx, someChar); err != nil || id != b {
		t.Errorf("Place = %d %v, want %d", id, err, b)
	}
}

// A heartbeat restarts the TTL window from the heartbeat, on the store's clock.
func TestHeartbeatRestartsTheTTLWindow(t *testing.T) {
	s, now := setup(t, "127.0.0.1:7777")
	ctx := context.Background()
	reg, err := s.Register(ctx, "192.0.2.5:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	*now = now.Add(10 * time.Second)
	if err := s.Heartbeat(ctx, reg); err != nil {
		t.Fatal(err)
	}
	*now = now.Add(10 * time.Second) // 20 s after Register, 10 s after the heartbeat
	if id, _, err := s.Place(ctx, someChar); err != nil || id != reg {
		t.Errorf("10 s after a heartbeat: Place = %d %v, want %d", id, err, reg)
	}
	*now = now.Add(6 * time.Second) // 16 s after the heartbeat
	if id, _, err := s.Place(ctx, someChar); err != nil || id != StaticServerID {
		t.Errorf("16 s after a heartbeat: Place = %d %v, want the static world", id, err)
	}
}

// A database failure is an error, never "unknown server", "no server" or the
// static world: worldd must retry it, not re-register, and the gateway must
// not send a player to the legacy server because the database blinked.
func TestDatabaseFailureIsNotAnAnswer(t *testing.T) {
	s, _ := setup(t, "127.0.0.1:7777")
	ctx := context.Background()
	reg, err := s.Register(ctx, "192.0.2.5:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	s.pool.Close()
	if err := s.Heartbeat(ctx, reg); err == nil || errors.Is(err, ErrUnknownServer) {
		t.Errorf("Heartbeat on a closed pool = %v, want a database error", err)
	}
	if id, addr, err := s.Place(ctx, someChar); err == nil || errors.Is(err, ErrNoServer) {
		t.Errorf("Place on a closed pool = %d %q %v, want a database error", id, addr, err)
	}
	if _, err := s.Register(ctx, "192.0.2.6:7001", nil); err == nil || errors.Is(err, ErrInvalid) {
		t.Errorf("Register on a closed pool = %v, want a database error", err)
	}
}

// C.Y.B + CoA

func TestHeartbeatTTLIsTheConfiguredOne(t *testing.T) {
	s, _ := setup(t, "")
	s2 := NewStore(s.pool, 20*time.Second, "127.0.0.1:1")
	if s2.HeartbeatTTL() != 20*time.Second {
		t.Errorf("HeartbeatTTL = %v, want 20s", s2.HeartbeatTTL())
	}
	// and Place honours it: 19 s of silence is still live
	s2.now = s.now
	ctx := context.Background()
	reg, err := s2.Register(ctx, "192.0.2.5:7001", nil)
	if err != nil {
		t.Fatal(err)
	}
	t0 := s2.now()
	s2.now = func() time.Time { return t0.Add(19 * time.Second) }
	if id, _, err := s2.Place(ctx, someChar); err != nil || id != reg {
		t.Errorf("19 s of silence with a 20 s TTL: Place = %d %v, want %d", id, err, reg)
	}
}

func TestStaticServerIDIsZero(t *testing.T) {
	if StaticServerID != 0 {
		t.Errorf("StaticServerID = %d; the proto, the schema and worldd all say 0", StaticServerID)
	}
}

// Addresses net.SplitHostPort accepts but a game client cannot connect to,
// among them what worldd would send if it registered its bind address.
func TestRegisterRejectsAddressesTheClientCannotDial(t *testing.T) {
	s, _ := setup(t, "")
	for _, address := range []string{":7777", "host:", ":", "host:abc", "host:0", "host:65536", "a b:7777", "0.0.0.0:7777", "[::]:7777"} {
		if _, err := s.Register(context.Background(), address, nil); !errors.Is(err, ErrInvalid) {
			t.Errorf("Register(%q) = %v, want ErrInvalid", address, err)
		}
	}
}

// ValidAddress is also what cmd/mesh checks AOC_MESH_STATIC_WORLD with.
func TestValidAddress(t *testing.T) {
	for _, address := range []string{
		"127.0.0.1:7777", "192.0.2.5:1", "localhost:7777", "world.example.com:65535",
		"[::1]:7777", "[2001:db8::1]:7777", "0.0.0.1:7777", "aoc_worldd_1:7777", "a-b.c:7777", "example.com.:7777", "World-1.Example.COM:7777",
	} {
		if err := ValidAddress(address); err != nil {
			t.Errorf("ValidAddress(%q) = %v, want nil", address, err)
		}
	}
	for _, address := range []string{
		"", "no-port", ":7777", ":", "host:", "host:abc", "host:0", "host:65536", "host:-1", "host:+1", "host:77 77",
		"0.0.0.0:7777", "[::]:7777", "[::ffff:0.0.0.0]:7777",
		"a b:7777", "a\tb:7777", "a\nb:7777", "a\x7fb:7777", "a\x00b:7777", "\xff:7777",
		"nb\u00a0sp:7777", "zw\u200bsp:7777", "\ufeffhost:7777", "m\u00fcnchen:7777", "a/b:7777", "[fe80::1%eth0]:7777",
		"host:0x50", "host:1_000",
		"nel\u0085x:7777", "c1\u009bx:7777", "em\u2003sp:7777", "lrm\u200ex:7777", "rlo\u202ex:7777",
	} {
		if err := ValidAddress(address); err == nil {
			t.Errorf("ValidAddress(%q) = nil, want an error", address)
		}
	}
}

// Equal heartbeats whose rows are not in id order: still the higher id.
func TestPlaceTieBreakIgnoresRowOrder(t *testing.T) {
	s, now := setup(t, "")
	ctx := context.Background()
	a, _ := s.Register(ctx, "192.0.2.5:7001", nil)
	b, _ := s.Register(ctx, "192.0.2.6:7001", nil)
	*now = now.Add(time.Second)
	// b's row is rewritten first, a's last: a is physically after b.
	if err := s.Heartbeat(ctx, b); err != nil {
		t.Fatal(err)
	}
	if err := s.Heartbeat(ctx, a); err != nil {
		t.Fatal(err)
	}
	if id, _, err := s.Place(ctx, someChar); err != nil || id != b {
		t.Errorf("Place = %d %v, want %d (the higher id of equal heartbeats)", id, err, b)
	}
}
