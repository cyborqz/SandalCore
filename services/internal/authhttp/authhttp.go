// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package authhttp serves the launcher's login API: POST
// /api/v1/auth/login and /api/v1/auth/register with {"username","password"}.
// The launcher treats a JSON body with access_token as success and shows
// "message" otherwise (src/launcher_main.cpp).
package authhttp

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"time"

	"aocrop/services/internal/account"
)

// Accounts is the part of account.Store the handlers use.
type Accounts interface {
	Register(ctx context.Context, username, password string) (account.Account, error)
	Authenticate(ctx context.Context, username, password string) (account.Account, error)
	IssueToken(ctx context.Context, accountID int64) (account.Token, error)
	TokenTTL() time.Duration
}

type credentials struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

func Handler(accounts Accounts) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
	})
	mux.HandleFunc("POST /api/v1/auth/login", func(w http.ResponseWriter, r *http.Request) {
		c, ok := readCredentials(w, r)
		if !ok {
			return
		}
		acc, err := accounts.Authenticate(r.Context(), c.Username, c.Password)
		if errors.Is(err, account.ErrInvalidCredentials) {
			writeError(w, http.StatusUnauthorized, "invalid_credentials", "Wrong username or password.")
			return
		}
		if err != nil {
			serverError(w, r, err)
			return
		}
		respondWithToken(w, r, accounts, acc, "login")
	})
	mux.HandleFunc("POST /api/v1/auth/register", func(w http.ResponseWriter, r *http.Request) {
		c, ok := readCredentials(w, r)
		if !ok {
			return
		}
		if err := account.ValidateCredentials(c.Username, c.Password); err != nil {
			writeError(w, http.StatusBadRequest, "invalid_request", err.Error())
			return
		}
		acc, err := accounts.Register(r.Context(), c.Username, c.Password)
		if errors.Is(err, account.ErrUsernameTaken) {
			writeError(w, http.StatusConflict, "username_taken", "That username is taken.")
			return
		}
		if errors.Is(err, account.ErrInvalidInput) {
			writeError(w, http.StatusBadRequest, "invalid_request", err.Error())
			return
		}
		if err != nil {
			serverError(w, r, err)
			return
		}
		respondWithToken(w, r, accounts, acc, "register")
	})
	return mux
}

// Cyb with C | O | A

func readCredentials(w http.ResponseWriter, r *http.Request) (credentials, bool) {
	var c credentials
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096))
	if err := dec.Decode(&c); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_request", "Body must be JSON with username and password.")
		return c, false
	}
	// Consume the whole bounded body: Decode alone accepts a valid prefix and
	// never notices another document, trailing junk or an oversized tail.
	if err := dec.Decode(new(any)); err != io.EOF {
		writeError(w, http.StatusBadRequest, "invalid_request", "Body must be JSON with username and password.")
		return c, false
	}
	return c, true
}

// serverError logs err and answers 500 without exposing it: store errors can
// name tables or hosts.
func serverError(w http.ResponseWriter, r *http.Request, err error) {
	slog.Error("auth request failed", "path", r.URL.Path, "err", err)
	writeError(w, http.StatusInternalServerError, "server_error", "The login server failed; try again.")
}

// respondWithToken answers a successful login or registration (event names
// which) with a new token.
func respondWithToken(w http.ResponseWriter, r *http.Request, accounts Accounts, acc account.Account, event string) {
	tok, err := accounts.IssueToken(r.Context(), acc.ID)
	if err != nil {
		serverError(w, r, err)
		return
	}
	slog.Info(event, "username", acc.Username)
	// RFC 6749 section 5.1: a token response must not be cached.
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, map[string]any{
		"access_token": tok.Value,
		"token_type":   "Bearer",
		"expires_in":   int(accounts.TokenTTL().Seconds()),
		"username":     acc.Username,
	})
}

func writeError(w http.ResponseWriter, status int, code, message string) {
	writeJSON(w, status, map[string]string{"error": code, "message": message})
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
