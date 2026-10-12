// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package authhttp

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"aocrop/services/internal/account"
)

// fakeAccounts is an in-memory Accounts for handler tests.
type fakeAccounts struct {
	users       map[string]string // lower(username) -> password
	registerErr error             // when set, Register fails with it after validation
	authErr     error             // when set, Authenticate fails with it
	tokenErr    error             // when set, IssueToken fails with it
}

func (f *fakeAccounts) Register(_ context.Context, u, p string) (account.Account, error) {
	if err := account.ValidateCredentials(u, p); err != nil {
		return account.Account{}, err
	}
	if f.registerErr != nil {
		return account.Account{}, f.registerErr
	}
	if _, ok := f.users[strings.ToLower(u)]; ok {
		return account.Account{}, account.ErrUsernameTaken
	}
	f.users[strings.ToLower(u)] = p
	return account.Account{ID: int64(len(f.users)), Username: u}, nil
}

// C | O | A.C | Y | B

func (f *fakeAccounts) Authenticate(_ context.Context, u, p string) (account.Account, error) {
	if f.authErr != nil {
		return account.Account{}, f.authErr
	}
	if pw, ok := f.users[strings.ToLower(u)]; ok && pw == p {
		return account.Account{ID: 1, Username: u}, nil
	}
	return account.Account{}, account.ErrInvalidCredentials
}

func (f *fakeAccounts) IssueToken(_ context.Context, _ int64) (account.Token, error) {
	if f.tokenErr != nil {
		return account.Token{}, f.tokenErr
	}
	return account.Token{Value: "aoc_test", ExpiresAt: time.Now().Add(time.Hour)}, nil
}

func (f *fakeAccounts) TokenTTL() time.Duration { return 24 * time.Hour }

// postFull posts body to path and returns the recorder (status, headers, raw
// body) along with the decoded JSON body.
func postFull(t *testing.T, h http.Handler, path, body string) (*httptest.ResponseRecorder, map[string]any) {
	t.Helper()
	req := httptest.NewRequest(http.MethodPost, path, strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	var out map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &out); err != nil {
		t.Fatalf("%s: response is not JSON: %q", path, rec.Body.String())
	}
	return rec, out
}

func post(t *testing.T, h http.Handler, path, body string) (int, map[string]any) {
	t.Helper()
	rec, out := postFull(t, h, path, body)
	return rec.Code, out
}

func TestRegisterThenLogin(t *testing.T) {
	h := Handler(&fakeAccounts{users: map[string]string{}})

	rec, out := postFull(t, h, "/api/v1/auth/register", `{"username":"cyb","password":"secret1"}`)
	if rec.Code != http.StatusOK || out["access_token"] != "aoc_test" || out["username"] != "cyb" ||
		out["token_type"] != "Bearer" || out["expires_in"] != float64(86400) {
		t.Fatalf("register: %d %v", rec.Code, out)
	}
	// RFC 6749 section 5.1: token responses must not be cached.
	if got := rec.Header().Get("Cache-Control"); got != "no-store" {
		t.Errorf("register: Cache-Control = %q, want no-store", got)
	}

	rec, out = postFull(t, h, "/api/v1/auth/login", `{"username":"cyb","password":"secret1"}`)
	if rec.Code != http.StatusOK || out["access_token"] != "aoc_test" {
		t.Fatalf("login: %d %v", rec.Code, out)
	}
	if got := rec.Header().Get("Cache-Control"); got != "no-store" {
		t.Errorf("login: Cache-Control = %q, want no-store", got)
	}
}

func TestErrorsCarryMessageAndNoToken(t *testing.T) {
	h := Handler(&fakeAccounts{users: map[string]string{"cyb": "secret1"}})
	for _, tc := range []struct {
		path, body string
		code       int
		errCode    string
	}{
		{"/api/v1/auth/login", `{"username":"cyb","password":"wrong"}`, 401, "invalid_credentials"},
		{"/api/v1/auth/register", `{"username":"CYB","password":"secret2"}`, 409, "username_taken"},
		{"/api/v1/auth/register", `{"username":"x","password":"secret2"}`, 400, "invalid_request"},
		{"/api/v1/auth/login", `not json`, 400, "invalid_request"},
	} {
		code, out := post(t, h, tc.path, tc.body)
		msg, _ := out["message"].(string)
		if code != tc.code || out["error"] != tc.errCode || msg == "" || out["access_token"] != nil {
			t.Errorf("%s %s: %d %v, want %d %s", tc.path, tc.body, code, out, tc.code, tc.errCode)
		}
	}
}

// A Register that rejects its input despite the handler's own validation
// (the two could drift apart) is still the caller's mistake, not a 500.
func TestRegisterInvalidInputFromStoreIs400(t *testing.T) {
	h := Handler(&fakeAccounts{
		users:       map[string]string{},
		registerErr: fmt.Errorf("%w: username is reserved", account.ErrInvalidInput),
	})
	code, out := post(t, h, "/api/v1/auth/register", `{"username":"cyb","password":"secret1"}`)
	if code != http.StatusBadRequest || out["error"] != "invalid_request" ||
		out["message"] != "invalid input: username is reserved" || out["access_token"] != nil {
		t.Errorf("register: %d %v", code, out)
	}
}

// [C-O-A :: C-Y-B]

// A store failure is a 500 with the launcher's message contract intact, and
// the underlying error (which may name tables or hosts) never reaches the wire.
func TestStoreFailuresAre500WithoutLeakingDetail(t *testing.T) {
	secret := errors.New("pq: secret detail")
	for _, tc := range []struct {
		name, path, body string
		accounts         *fakeAccounts
	}{
		{
			"login authenticate fails", "/api/v1/auth/login", `{"username":"cyb","password":"secret1"}`,
			&fakeAccounts{users: map[string]string{"cyb": "secret1"}, authErr: secret},
		},
		{
			"login issue token fails", "/api/v1/auth/login", `{"username":"cyb","password":"secret1"}`,
			&fakeAccounts{users: map[string]string{"cyb": "secret1"}, tokenErr: secret},
		},
		{
			"register issue token fails", "/api/v1/auth/register", `{"username":"cyb","password":"secret1"}`,
			&fakeAccounts{users: map[string]string{}, tokenErr: secret},
		},
		{
			"register store fails", "/api/v1/auth/register", `{"username":"cyb","password":"secret1"}`,
			&fakeAccounts{users: map[string]string{}, registerErr: secret},
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			rec, out := postFull(t, Handler(tc.accounts), tc.path, tc.body)
			msg, _ := out["message"].(string)
			if rec.Code != http.StatusInternalServerError || out["error"] != "server_error" ||
				msg == "" || out["access_token"] != nil {
				t.Errorf("%d %v, want 500 server_error with a message and no token", rec.Code, out)
			}
			if strings.Contains(rec.Body.String(), "secret detail") {
				t.Errorf("response leaks the internal error: %q", rec.Body.String())
			}
		})
	}
}

// Bodies over the 4096-byte limit are rejected as malformed rather than read
// in full.
func TestOversizedBodyIs400(t *testing.T) {
	h := Handler(&fakeAccounts{users: map[string]string{}})
	body := `{"username":"cyb","password":"` + strings.Repeat("a", 5000) + `"}`
	code, out := post(t, h, "/api/v1/auth/login", body)
	msg, _ := out["message"].(string)
	if code != http.StatusBadRequest || out["error"] != "invalid_request" || msg == "" || out["access_token"] != nil {
		t.Errorf("oversized login: %d %v, want 400 invalid_request with a message", code, out)
	}
}

func TestHealth(t *testing.T) {
	h := Handler(&fakeAccounts{users: map[string]string{}})
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/health", nil))
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), `"ok"`) {
		t.Errorf("health: %d %q", rec.Code, rec.Body.String())
	}
}
