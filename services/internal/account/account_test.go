// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package account

import (
	"bytes"
	"context"
	"crypto/sha256"
	"errors"
	"fmt"
	"strings"
	"testing"
	"time"

	"golang.org/x/crypto/bcrypt"

	"aocrop/services/internal/db"
	"aocrop/services/internal/dbtest"
)

func TestValidateCredentials(t *testing.T) {
	for _, tc := range []struct {
		user, pass string
		ok         bool
	}{
		{"cyb", "test", true},
		{"test222", "hunter22", true},
		{"a.b-c_d", "pass", true},
		{"ab", "pass", false},       // username too short
		{"bad name", "pass", false}, // space
		{"cyb", "abc", false},       // password too short
		{string(make([]byte, 33)), "pass", false},
		{"cyb", strings.Repeat("p", 72), true},  // bcrypt takes at most 72 bytes
		{"cyb", strings.Repeat("p", 73), false}, // one byte over
	} {
		err := ValidateCredentials(tc.user, tc.pass)
		if err != nil && !errors.Is(err, ErrInvalidInput) {
			t.Errorf("ValidateCredentials(%q, %q) = %v, want it to wrap ErrInvalidInput", tc.user, tc.pass, err)
		}
		if (err == nil) != tc.ok {
			t.Errorf("ValidateCredentials(%q, %q) = %v, want ok=%v", tc.user, tc.pass, err, tc.ok)
		}
	}
}

func newStore(t *testing.T) *Store {
	pool := dbtest.Pool(t)
	if err := db.Migrate(context.Background(), pool); err != nil {
		t.Fatal(err)
	}
	return NewStore(pool, time.Hour)
}

// == C:Y:B | CoA ==

func TestRegisterAuthenticateAndTokens(t *testing.T) {
	s := newStore(t)
	ctx := context.Background()

	acc, err := s.Register(ctx, "Cyb", "secret1")
	if err != nil {
		t.Fatalf("Register: %v", err)
	}
	if acc.ID == 0 || acc.Username != "Cyb" {
		t.Fatalf("Register returned %+v", acc)
	}
	if _, err := s.Register(ctx, "cyb", "other1"); !errors.Is(err, ErrUsernameTaken) {
		t.Errorf("duplicate (case-insensitive) Register: %v, want ErrUsernameTaken", err)
	}

	got, err := s.Authenticate(ctx, "CYB", "secret1")
	if err != nil || got.ID != acc.ID {
		t.Fatalf("Authenticate: %+v %v", got, err)
	}
	if _, err := s.Authenticate(ctx, "cyb", "wrong"); !errors.Is(err, ErrInvalidCredentials) {
		t.Errorf("wrong password: %v", err)
	}
	if _, err := s.Authenticate(ctx, "nobody", "secret1"); !errors.Is(err, ErrInvalidCredentials) {
		t.Errorf("unknown user: %v", err)
	}

	tok, err := s.IssueToken(ctx, acc.ID)
	if err != nil {
		t.Fatalf("IssueToken: %v", err)
	}
	if len(tok.Value) != 68 || tok.Value[:4] != "aoc_" {
		t.Errorf("token %q: want aoc_ + 64 hex", tok.Value)
	}
	owner, err := s.ValidateToken(ctx, tok.Value)
	if err != nil || owner.ID != acc.ID || owner.Username != "Cyb" {
		t.Fatalf("ValidateToken: %+v %v", owner, err)
	}
	if _, err := s.ValidateToken(ctx, "aoc_nope"); !errors.Is(err, ErrInvalidToken) {
		t.Errorf("unknown token: %v", err)
	}
}

func TestExpiredTokenIsRejected(t *testing.T) {
	s := newStore(t)
	ctx := context.Background()
	acc, err := s.Register(ctx, "tester", "secret1")
	if err != nil {
		t.Fatal(err)
	}
	s.now = func() time.Time { return time.Now().Add(-2 * time.Hour) } // issue in the past
	tok, err := s.IssueToken(ctx, acc.ID)
	if err != nil {
		t.Fatal(err)
	}
	s.now = time.Now
	if _, err := s.ValidateToken(ctx, tok.Value); !errors.Is(err, ErrInvalidToken) {
		t.Errorf("expired token: %v, want ErrInvalidToken", err)
	}
}

func TestValidateCredentialsMessages(t *testing.T) {
	err := ValidateCredentials("cyb", strings.Repeat("p", 73))
	if err == nil || err.Error() != "invalid input: password must be 4-72 bytes" {
		t.Errorf("password message = %v", err)
	}
	err = ValidateCredentials("ab", "pass")
	if err == nil || !strings.HasPrefix(err.Error(), "invalid input: username must be 3-32 ") {
		t.Errorf("username message = %v", err)
	}
}

func countRows(t *testing.T, s *Store, table string) int {
	t.Helper()
	var n int
	if err := s.pool.QueryRow(context.Background(), "SELECT count(*) FROM "+table).Scan(&n); err != nil {
		t.Fatal(err)
	}
	return n
}

func TestRegisterRejectsInvalidCredentials(t *testing.T) {
	s := newStore(t)
	ctx := context.Background()
	for _, tc := range []struct{ user, pass string }{
		{"ab", "secret1"},
		{"bad name", "secret1"},
		{"cyb", "abc"},
		{"cyb", strings.Repeat("p", 73)},
	} {
		if _, err := s.Register(ctx, tc.user, tc.pass); !errors.Is(err, ErrInvalidInput) {
			t.Errorf("Register(%q, %d bytes) = %v, want ErrInvalidInput", tc.user, len(tc.pass), err)
		}
	}
	if n := countRows(t, s, "accounts"); n != 0 {
		t.Errorf("%d account rows after invalid registrations, want 0", n)
	}
}

func TestAuthenticateRejectsBadShapeWithoutDBError(t *testing.T) {
	s := newStore(t)
	ctx := context.Background()
	if _, err := s.Register(ctx, "kyle", "secret1"); err != nil {
		t.Fatal(err)
	}
	long := strings.Repeat("p", 72)
	if _, err := s.Register(ctx, "longpass", long); err != nil {
		t.Fatal(err)
	}

	for name, user := range map[string]string{
		"NUL byte":              "ky\x00le",
		"Kelvin sign lookalike": "\xE2\x84\xAAyle", // U+212A: lower() folds it to "k"
		"Cyrillic lookalike":    "\xD0\x9Ayle",     // U+041A
		"trailing space":        "kyle ",
		"empty":                 "",
	} {
		if _, err := s.Authenticate(ctx, user, "secret1"); !errors.Is(err, ErrInvalidCredentials) {
			t.Errorf("%s: Authenticate(%q) = %v, want ErrInvalidCredentials", name, user, err)
		}
	}

	// bcrypt only looks at the first 72 bytes; a longer password that starts
	// with the real one must not be accepted.
	if _, err := s.Authenticate(ctx, "longpass", long); err != nil {
		t.Fatalf("72-byte password: %v", err)
	}
	if _, err := s.Authenticate(ctx, "longpass", long+"x"); !errors.Is(err, ErrInvalidCredentials) {
		t.Errorf("73-byte password with the real 72-byte prefix: %v, want ErrInvalidCredentials", err)
	}
}

func TestAuthenticateSurfacesCorruptHash(t *testing.T) {
	s := newStore(t)
	ctx := context.Background()
	acc, err := s.Register(ctx, "cyb", "secret1")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.pool.Exec(ctx, "UPDATE accounts SET password_hash = 'not-a-bcrypt-hash' WHERE id = $1", acc.ID); err != nil {
		t.Fatal(err)
	}
	_, err = s.Authenticate(ctx, "cyb", "secret1")
	if err == nil || errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("corrupt stored hash: %v, want a non-credentials error", err)
	}
	if want := fmt.Sprintf("account %d: password hash: ", acc.ID); !strings.HasPrefix(err.Error(), want) {
		t.Errorf("error %q does not start with %q", err, want)
	}
}

// [C.O.A x C_Y_B]

func TestFindByUsername(t *testing.T) {
	s := newStore(t)
	ctx := context.Background()
	acc, err := s.Register(ctx, "Cyb", "secret1")
	if err != nil {
		t.Fatal(err)
	}
	got, err := s.FindByUsername(ctx, "CYB")
	if err != nil || got != acc {
		t.Fatalf("FindByUsername(CYB) = %+v, %v; want %+v", got, err, acc)
	}
	if _, err := s.Register(ctx, "kyle", "secret1"); err != nil {
		t.Fatal(err)
	}
	// U+212A KELVIN SIGN: Postgres lower() folds it to "k", so it would find kyle.
	for _, name := range []string{"nobody", "bad name", "ky\x00le", "\xE2\x84\xAAyle", ""} {
		if _, err := s.FindByUsername(ctx, name); !errors.Is(err, ErrAccountNotFound) {
			t.Errorf("FindByUsername(%q) = %v, want ErrAccountNotFound", name, err)
		}
	}
}

func TestPasswordsAreStoredAsBcrypt(t *testing.T) {
	s := newStore(t)
	ctx := context.Background()
	acc, err := s.Register(ctx, "cyb", "secret1")
	if err != nil {
		t.Fatal(err)
	}
	var hash string
	if err := s.pool.QueryRow(ctx, "SELECT password_hash FROM accounts WHERE id = $1", acc.ID).Scan(&hash); err != nil {
		t.Fatal(err)
	}
	if hash == "secret1" || strings.Contains(hash, "secret1") {
		t.Errorf("password stored in the clear: %q", hash)
	}
	cost, err := bcrypt.Cost([]byte(hash))
	if err != nil || cost != bcrypt.DefaultCost {
		t.Errorf("bcrypt.Cost(%q) = %d, %v; want %d", hash, cost, err, bcrypt.DefaultCost)
	}
}

func TestTokensAreStoredHashedAndUnique(t *testing.T) {
	s := newStore(t)
	ctx := context.Background()
	acc, err := s.Register(ctx, "cyb", "secret1")
	if err != nil {
		t.Fatal(err)
	}
	tok, err := s.IssueToken(ctx, acc.ID)
	if err != nil {
		t.Fatal(err)
	}
	other, err := s.IssueToken(ctx, acc.ID)
	if err != nil {
		t.Fatal(err)
	}
	if tok.Value == other.Value {
		t.Fatal("two IssueToken calls returned the same token")
	}

	want := sha256.Sum256([]byte(tok.Value))
	var stored []byte
	var rowText string
	if err := s.pool.QueryRow(ctx,
		"SELECT token_hash, t::text FROM login_tokens t WHERE token_hash = $1", want[:]).Scan(&stored, &rowText); err != nil {
		t.Fatalf("no login_tokens row with token_hash = sha256(token): %v", err)
	}
	if !bytes.Equal(stored, want[:]) {
		t.Errorf("token_hash = %x, want %x", stored, want)
	}
	if strings.Contains(rowText, tok.Value) || strings.Contains(rowText, tok.Value[len("aoc_"):]) {
		t.Errorf("raw token appears in the stored row: %s", rowText)
	}
	if n := countRows(t, s, "login_tokens"); n != 2 {
		t.Errorf("%d login_tokens rows, want 2", n)
	}
}

// An unknown username must cost about one bcrypt comparison, like a wrong
// password does. Without the dummy compare the lookup alone takes well under a
// millisecond.
func TestAuthenticateUnknownUserCostsABcryptCompare(t *testing.T) {
	s := newStore(t)
	ctx := context.Background()
	if _, err := s.Register(ctx, "cyb", "secret1"); err != nil {
		t.Fatal(err)
	}

	hash, err := bcrypt.GenerateFromPassword([]byte("secret1"), bcrypt.DefaultCost)
	if err != nil {
		t.Fatal(err)
	}
	start := time.Now()
	if bcrypt.CompareHashAndPassword(hash, []byte("wrong-pw")) == nil {
		t.Fatal("wrong password matched")
	}
	cost := time.Since(start)

	dummyHash() // warm the lazily computed hash
	start = time.Now()
	if _, err := s.Authenticate(ctx, "nobody", "secret1"); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("unknown user: %v", err)
	}
	if took := time.Since(start); took < cost/2 {
		t.Errorf("unknown user took %v, want at least half of one bcrypt compare (%v): the dummy compare is missing", took, cost)
	}
}

func TestPurgeExpiredTokens(t *testing.T) {
	s := newStore(t)
	ctx := context.Background()
	acc, err := s.Register(ctx, "tester", "secret1")
	if err != nil {
		t.Fatal(err)
	}
	s.now = func() time.Time { return time.Now().Add(-2 * time.Hour) } // expired an hour ago
	if _, err := s.IssueToken(ctx, acc.ID); err != nil {
		t.Fatal(err)
	}
	s.now = time.Now
	live, err := s.IssueToken(ctx, acc.ID)
	if err != nil {
		t.Fatal(err)
	}

	n, err := s.PurgeExpiredTokens(ctx)
	if err != nil || n != 1 {
		t.Fatalf("PurgeExpiredTokens = %d, %v; want 1 removed", n, err)
	}
	if rows := countRows(t, s, "login_tokens"); rows != 1 {
		t.Errorf("login_tokens rows after purge = %d, want 1", rows)
	}
	if _, err := s.ValidateToken(ctx, live.Value); err != nil {
		t.Errorf("live token after purge: %v", err)
	}
}
