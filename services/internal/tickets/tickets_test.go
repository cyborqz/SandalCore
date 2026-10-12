// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package tickets

import (
	"context"
	"errors"
	"math"
	"regexp"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgconn"

	"aocrop/services/internal/account"
	"aocrop/services/internal/character"
	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"
)

type fixture struct {
	s         *Store
	now       *time.Time
	accountID int64
	charID    string
	chars     *character.Store
}

// C | O | A with Cyb

// setup returns a store (2-minute tickets, a clock the test moves) with one
// account owning one character.
func setup(t *testing.T) fixture {
	t.Helper()
	pool := dbtest.Pool(t)
	ctx := context.Background()
	if err := db.Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	acc, err := account.NewStore(pool, 0).Register(ctx, "owner", "secret1")
	if err != nil {
		t.Fatal(err)
	}
	chars := character.NewStore(pool)
	c, err := chars.Create(ctx, character.New{AccountID: acc.ID, Name: "cyb", ClassName: "Bard"})
	if err != nil {
		t.Fatal(err)
	}
	s := NewStore(pool, 2*time.Minute)
	now := time.Date(2026, 9, 29, 12, 0, 0, 0, time.UTC)
	s.now = func() time.Time { return now }
	return fixture{s: s, now: &now, accountID: acc.ID, charID: c.ID, chars: chars}
}

var ticketShape = regexp.MustCompile(`^[0-9a-f]{32}$`)

func TestIssueThenRedeemOnce(t *testing.T) {
	f := setup(t)
	ctx := context.Background()
	ticket, err := f.s.Issue(ctx, f.accountID, f.charID, 60)
	if err != nil {
		t.Fatal(err)
	}
	if !ticketShape.MatchString(ticket) {
		t.Errorf("ticket %q is not 32 lowercase hex digits (the client carries it as a GUID)", ticket)
	}
	got, err := f.s.Redeem(ctx, ticket, f.charID, 60)
	if err != nil || got != f.accountID {
		t.Fatalf("Redeem = %d %v, want account %d", got, err, f.accountID)
	}
	if _, err := f.s.Redeem(ctx, ticket, f.charID, 60); !errors.Is(err, ErrInvalid) {
		t.Errorf("second Redeem = %v, want ErrInvalid", err)
	}
}

func TestTicketsDiffer(t *testing.T) {
	f := setup(t)
	a, err := f.s.Issue(context.Background(), f.accountID, f.charID, 0)
	if err != nil {
		t.Fatal(err)
	}
	b, err := f.s.Issue(context.Background(), f.accountID, f.charID, 0)
	if err != nil || a == b {
		t.Errorf("two tickets %q and %q (%v), want different", a, b, err)
	}
}

// A ticket is only issued for the account's own, existing character.
func TestIssueRequiresTheAccountsCharacter(t *testing.T) {
	f := setup(t)
	ctx := context.Background()
	other, err := account.NewStore(f.s.pool, 0).Register(ctx, "other", "secret1")
	if err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		name      string
		accountID int64
		char      string
		server    uint32
	}{
		{"another account's character", other.ID, f.charID, 60},
		{"no such character", f.accountID, "ffffffffffffffffffffffffffffffff", 60},
		{"malformed character id", f.accountID, "a\x00b", 60},
		{"server id out of range", f.accountID, f.charID, math.MaxInt32 + 1},
	} {
		if _, err := f.s.Issue(ctx, tc.accountID, tc.char, tc.server); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: Issue = %v, want ErrInvalid", tc.name, err)
		}
	}
}

// A ticket is for one character on one server; a wrong attempt does not
// spend it.
func TestRedeemChecksCharacterAndServer(t *testing.T) {
	f := setup(t)
	ctx := context.Background()
	ticket, err := f.s.Issue(ctx, f.accountID, f.charID, 60)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := f.s.Redeem(ctx, ticket, "ffffffffffffffffffffffffffffffff", 60); !errors.Is(err, ErrInvalid) {
		t.Errorf("other character: %v, want ErrInvalid", err)
	}
	if _, err := f.s.Redeem(ctx, ticket, f.charID, 61); !errors.Is(err, ErrInvalid) {
		t.Errorf("other server: %v, want ErrInvalid", err)
	}
	if got, err := f.s.Redeem(ctx, ticket, f.charID, 60); err != nil || got != f.accountID {
		t.Errorf("right character and server afterwards: %d %v", got, err)
	}
}

func TestRedeemAfterExpiry(t *testing.T) {
	f := setup(t)
	ctx := context.Background()
	ticket, err := f.s.Issue(ctx, f.accountID, f.charID, 60)
	if err != nil {
		t.Fatal(err)
	}
	*f.now = f.now.Add(2*time.Minute + time.Second)
	if _, err := f.s.Redeem(ctx, ticket, f.charID, 60); !errors.Is(err, ErrInvalid) {
		t.Errorf("expired ticket: %v, want ErrInvalid", err)
	}
}

// Malformed input is refused before PostgreSQL sees it (NUL and invalid UTF-8
// would otherwise come back as raw database errors).
func TestRedeemMalformedInput(t *testing.T) {
	f := setup(t)
	ctx := context.Background()
	good := "0123456789abcdef0123456789abcdef"
	for _, tc := range []struct {
		ticket, char string
		server       uint32
	}{
		{"", f.charID, 60}, {"NOT-HEX", f.charID, 60}, {"0123456789ABCDEF0123456789ABCDEF", f.charID, 60},
		{"0123456789abcdef0123456789abcde\x00", f.charID, 60}, {good, "a\x00b", 60}, {good, "\xff", 60},
		{good, f.charID, math.MaxInt32 + 1},
	} {
		if _, err := f.s.Redeem(ctx, tc.ticket, tc.char, tc.server); !errors.Is(err, ErrInvalid) {
			t.Errorf("Redeem(%q, %q, %d) = %v, want ErrInvalid", tc.ticket, tc.char, tc.server, err)
		}
	}

	// Server 0 is the static world, which never redeems tickets (it is also
	// what an unset proto3 server_id looks like): not even a ticket issued for
	// it is redeemable.
	ticket, err := f.s.Issue(ctx, f.accountID, f.charID, 0)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := f.s.Redeem(ctx, ticket, f.charID, 0); !errors.Is(err, ErrInvalid) {
		t.Errorf("Redeem for server 0 = %v, want ErrInvalid", err)
	}
}

func TestPurgeRemovesOnlyExpiredTickets(t *testing.T) {
	f := setup(t)
	ctx := context.Background()
	if _, err := f.s.Issue(ctx, f.accountID, f.charID, 60); err != nil { // expires at 12:02
		t.Fatal(err)
	}
	*f.now = f.now.Add(time.Minute)
	live, err := f.s.Issue(ctx, f.accountID, f.charID, 60) // expires at 12:03
	if err != nil {
		t.Fatal(err)
	}
	*f.now = f.now.Add(time.Minute + time.Second) // 12:02:01
	if n, err := f.s.Purge(ctx); err != nil || n != 1 {
		t.Fatalf("Purge = %d %v, want 1", n, err)
	}
	if _, err := f.s.Redeem(ctx, live, f.charID, 60); err != nil {
		t.Errorf("live ticket after purge: %v", err)
	}
}

// built with C | O | A and C | Y | B

func TestTicketsGoWithTheirCharacter(t *testing.T) {
	f := setup(t)
	ctx := context.Background()
	ticket, err := f.s.Issue(ctx, f.accountID, f.charID, 60)
	if err != nil {
		t.Fatal(err)
	}
	if err := f.chars.Delete(ctx, f.accountID, f.charID); err != nil {
		t.Fatal(err)
	}
	if _, err := f.s.Redeem(ctx, ticket, f.charID, 60); !errors.Is(err, ErrInvalid) {
		t.Errorf("ticket of a deleted character: %v, want ErrInvalid", err)
	}
}

// A well-formed ticket that was never issued must not redeem another ticket
// for the same character and server (the ticket column is the credential),
// and must not spend the real one.
func TestRedeemUnknownTicket(t *testing.T) {
	f := setup(t)
	ctx := context.Background()
	real, err := f.s.Issue(ctx, f.accountID, f.charID, 60)
	if err != nil {
		t.Fatal(err)
	}
	const forged = "0123456789abcdef0123456789abcdef"
	if _, err := f.s.Redeem(ctx, forged, f.charID, 60); !errors.Is(err, ErrInvalid) {
		t.Errorf("never-issued ticket: %v, want ErrInvalid", err)
	}
	if got, err := f.s.Redeem(ctx, real, f.charID, 60); err != nil || got != f.accountID {
		t.Errorf("real ticket after a forged attempt: %d %v", got, err)
	}
}

// Server 0 (the static world, which has no world_servers row) can have a
// ticket issued for it, but Redeem never honours server 0 (it is also what an
// unset proto3 field looks like); the largest server id the INT column holds
// works end to end, and Redeem refuses one more.
func TestServerIDBoundary(t *testing.T) {
	f := setup(t)
	ctx := context.Background()
	static, err := f.s.Issue(ctx, f.accountID, f.charID, 0)
	if err != nil {
		t.Fatalf("Issue for the static world: %v", err)
	}
	if _, err := f.s.Redeem(ctx, static, f.charID, 1); !errors.Is(err, ErrInvalid) {
		t.Errorf("a static-world ticket redeemed by server 1: %v, want ErrInvalid", err)
	}
	if _, err := f.s.Redeem(ctx, static, f.charID, 0); !errors.Is(err, ErrInvalid) {
		t.Errorf("Redeem at server 0 = %v, want ErrInvalid (server 0 never redeems)", err)
	}
	ticket, err := f.s.Issue(ctx, f.accountID, f.charID, math.MaxInt32)
	if err != nil {
		t.Fatalf("Issue at MaxInt32: %v", err)
	}
	if _, err := f.s.Redeem(ctx, ticket, f.charID, math.MaxInt32+1); !errors.Is(err, ErrInvalid) {
		t.Errorf("Redeem at MaxInt32+1: %v, want ErrInvalid", err)
	}
	if got, err := f.s.Redeem(ctx, ticket, f.charID, math.MaxInt32); err != nil || got != f.accountID {
		t.Errorf("Redeem at MaxInt32: %d %v", got, err)
	}
}

// All 128 bits are random: over a run of tickets no byte position is constant.
func TestTicketsUseAll128Bits(t *testing.T) {
	f := setup(t)
	const n = 24
	var seen [16]map[string]bool
	for i := range seen {
		seen[i] = map[string]bool{}
	}
	for i := 0; i < n; i++ {
		tk, err := f.s.Issue(context.Background(), f.accountID, f.charID, 60)
		if err != nil {
			t.Fatal(err)
		}
		for b := 0; b < 16; b++ {
			seen[b][tk[2*b:2*b+2]] = true
		}
	}
	for b, m := range seen {
		if len(m) < 2 {
			t.Errorf("byte %d of %d tickets never varies: not random", b, n)
		}
	}
}

// ~ Cyb :: C O A ~

// The ttl NewStore is given is the one used, to the microsecond (the column's
// resolution): valid one tick before expires_at, refused at it; Purge takes a
// ticket at exactly its expires_at (spent or not) and keeps it one tick before.
func TestExpiryAndPurgeBoundary(t *testing.T) {
	f := setup(t)
	ctx := context.Background()
	const ttl = 45 * time.Second
	s := NewStore(f.s.pool, ttl)
	t0 := time.Date(2026, 9, 29, 12, 0, 0, 0, time.UTC)
	now := t0
	s.now = func() time.Time { return now }

	var tk [4]string
	for i := range tk {
		var err error
		if tk[i], err = s.Issue(ctx, f.accountID, f.charID, 60); err != nil {
			t.Fatal(err)
		}
	}
	now = t0.Add(ttl - time.Microsecond)
	if got, err := s.Redeem(ctx, tk[0], f.charID, 60); err != nil || got != f.accountID {
		t.Fatalf("Redeem one tick before expires_at: %d %v", got, err)
	}
	if n, err := s.Purge(ctx); err != nil || n != 0 {
		t.Fatalf("Purge one tick before expires_at = %d %v, want 0", n, err)
	}
	now = t0.Add(ttl)
	if _, err := s.Redeem(ctx, tk[1], f.charID, 60); !errors.Is(err, ErrInvalid) {
		t.Errorf("Redeem at expires_at: %v, want ErrInvalid", err)
	}
	if n, err := s.Purge(ctx); err != nil || n != 4 {
		t.Errorf("Purge at expires_at = %d %v, want all 4 (one of them spent)", n, err)
	}
}

// A database failure is an error, never ErrInvalid (the gateway answers
// Internal for it, not PermissionDenied) and never a successful redeem of
// account 0.
func TestDatabaseFailuresAreNotInvalid(t *testing.T) {
	f := setup(t)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	const tk = "0123456789abcdef0123456789abcdef"
	if _, err := f.s.Issue(ctx, f.accountID, f.charID, 60); err == nil || errors.Is(err, ErrInvalid) {
		t.Errorf("Issue on a dead context = %v, want a non-ErrInvalid error", err)
	}
	if id, err := f.s.Redeem(ctx, tk, f.charID, 60); err == nil || errors.Is(err, ErrInvalid) || id != 0 {
		t.Errorf("Redeem on a dead context = %d %v, want 0 and a non-ErrInvalid error", id, err)
	}
	if _, err := f.s.Purge(ctx); err == nil {
		t.Errorf("Purge on a dead context succeeded")
	}
}

// Only the foreign-key race (SQLSTATE 23503) is ErrInvalid. A ttl of zero
// breaks the expires_after_created check (23514): that is a fault, not "not
// your character".
func TestOtherConstraintFailuresAreNotInvalid(t *testing.T) {
	f := setup(t)
	s := NewStore(f.s.pool, 0)
	_, err := s.Issue(context.Background(), f.accountID, f.charID, 60)
	var pgErr *pgconn.PgError
	if errors.Is(err, ErrInvalid) || !errors.As(err, &pgErr) || pgErr.Code != "23514" {
		t.Errorf("Issue with ttl 0 = %v, want the 23514 check violation", err)
	}
}

// The shape guards are anchored: a valid-looking prefix or suffix with NUL,
// a newline or invalid UTF-8 around it must not reach PostgreSQL either.
func TestShapeGuardsAreAnchored(t *testing.T) {
	f := setup(t)
	ctx := context.Background()
	good := "0123456789abcdef0123456789abcdef"
	other, err := account.NewStore(f.s.pool, 0).Register(ctx, "second", "secret1")
	if err != nil {
		t.Fatal(err)
	}
	for _, bad := range []string{good + "\x00", "\x00" + good, good + "\n", "\n" + good, good + "\xff", "\xff" + good, good + "0", "0" + good} {
		if _, err := f.s.Redeem(ctx, bad, f.charID, 60); !errors.Is(err, ErrInvalid) {
			t.Errorf("Redeem ticket %q = %v, want ErrInvalid", bad, err)
		}
		if _, err := f.s.Redeem(ctx, good, bad, 60); !errors.Is(err, ErrInvalid) {
			t.Errorf("Redeem character %q = %v, want ErrInvalid", bad, err)
		}
		if _, err := f.s.Issue(ctx, other.ID, bad, 60); !errors.Is(err, ErrInvalid) {
			t.Errorf("Issue character %q = %v, want ErrInvalid", bad, err)
		}
	}
}

// Issue while another transaction holds an uncommitted delete of the
// character: the insert's foreign-key check waits for it. If the delete
// commits the character is gone, and Issue says so (ErrInvalid, not a raw
// 23503); if it rolls back the ticket is issued.
func TestIssueWhileTheCharacterIsBeingDeleted(t *testing.T) {
	for _, tc := range []struct {
		name   string
		commit bool
	}{{"delete commits", true}, {"delete rolls back", false}} {
		t.Run(tc.name, func(t *testing.T) {
			f := setup(t)
			ctx := context.Background()
			tx, err := f.s.pool.Begin(ctx)
			if err != nil {
				t.Fatal(err)
			}
			defer tx.Rollback(ctx)
			if _, err := tx.Exec(ctx, "DELETE FROM characters WHERE id = $1", f.charID); err != nil {
				t.Fatal(err)
			}
			type result struct {
				ticket string
				err    error
			}
			done := make(chan result, 1)
			go func() {
				tk, err := f.s.Issue(ctx, f.accountID, f.charID, 60)
				done <- result{tk, err}
			}()
			// Wait for Issue to block on the lock (this test's connections
			// all carry the schema name as application_name).
			deadline := time.Now().Add(10 * time.Second)
			for {
				var waiting int
				if err := f.s.pool.QueryRow(ctx, `SELECT count(*) FROM pg_stat_activity
					WHERE application_name = current_setting('application_name')
					  AND wait_event_type = 'Lock' AND pid <> pg_backend_pid()`).Scan(&waiting); err != nil {
					t.Fatal(err)
				}
				if waiting > 0 {
					break
				}
				select {
				case r := <-done:
					t.Fatalf("Issue returned (%q, %v) without waiting for the delete", r.ticket, r.err)
				default:
				}
				if time.Now().After(deadline) {
					t.Fatal("Issue never blocked on the uncommitted delete")
				}
				time.Sleep(10 * time.Millisecond)
			}
			if tc.commit {
				err = tx.Commit(ctx)
			} else {
				err = tx.Rollback(ctx)
			}
			if err != nil {
				t.Fatal(err)
			}
			r := <-done
			if tc.commit {
				if !errors.Is(r.err, ErrInvalid) {
					t.Errorf("Issue = %q %v, want ErrInvalid", r.ticket, r.err)
				}
				return
			}
			if r.err != nil {
				t.Fatalf("Issue after the delete rolled back: %v", r.err)
			}
			if got, err := f.s.Redeem(ctx, r.ticket, f.charID, 60); err != nil || got != f.accountID {
				t.Errorf("Redeem = %d %v", got, err)
			}
		})
	}
}

// C | O | A : C Y B

// Several world servers (or a replayed UDP handshake) redeem the same ticket
// at the same moment: exactly one gets the account, the rest ErrInvalid. The
// redeemers are lined up behind a row lock so they all run the statement
// against the same unspent row; a check-then-update would let every one win.
func TestConcurrentRedeemSpendsOnce(t *testing.T) {
	f := setup(t)
	ctx := context.Background()
	ticket, err := f.s.Issue(ctx, f.accountID, f.charID, 60)
	if err != nil {
		t.Fatal(err)
	}
	n := int(f.s.pool.Config().MaxConns) - 2 // one for the lock holder, one for the poller
	if n > 8 {
		n = 8
	}
	if n < 2 {
		t.Skipf("pool too small (%d connections)", f.s.pool.Config().MaxConns)
	}
	tx, err := f.s.pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	if _, err := tx.Exec(ctx, "SELECT 1 FROM sessions WHERE ticket = $1 FOR UPDATE", ticket); err != nil {
		t.Fatal(err)
	}
	results := make(chan error, n)
	for i := 0; i < n; i++ {
		go func() {
			got, err := f.s.Redeem(ctx, ticket, f.charID, 60)
			if err == nil && got != f.accountID {
				err = errors.New("wrong account")
			}
			results <- err
		}()
	}
	deadline := time.Now().Add(10 * time.Second)
	for {
		var waiting int
		if err := f.s.pool.QueryRow(ctx, `SELECT count(*) FROM pg_stat_activity
			WHERE application_name = current_setting('application_name')
			  AND wait_event_type = 'Lock' AND pid <> pg_backend_pid()`).Scan(&waiting); err != nil {
			t.Fatal(err)
		}
		if waiting == n {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("only %d of %d redeemers waiting on the row lock", waiting, n)
		}
		time.Sleep(10 * time.Millisecond)
	}
	if err := tx.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	wins := 0
	for i := 0; i < n; i++ {
		switch err := <-results; {
		case err == nil:
			wins++
		case errors.Is(err, ErrInvalid):
		default:
			t.Errorf("Redeem = %v", err)
		}
	}
	if wins != 1 {
		t.Errorf("%d of %d concurrent redeems won, want exactly 1", wins, n)
	}
}
