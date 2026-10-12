// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package account owns player accounts and the launcher's login tokens.
package account

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"regexp"
	"sync"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
	"golang.org/x/crypto/bcrypt"
)

var (
	ErrUsernameTaken      = errors.New("username taken")
	ErrInvalidCredentials = errors.New("invalid username or password")
	ErrInvalidToken       = errors.New("invalid or expired token")
	ErrAccountNotFound    = errors.New("account not found")
	ErrInvalidInput       = errors.New("invalid input")
)

type Account struct {
	ID       int64
	Username string
}

type Token struct {
	Value     string
	ExpiresAt time.Time
}

var usernamePattern = regexp.MustCompile(`^[A-Za-z0-9_.-]{3,32}$`)

// maxPasswordBytes is bcrypt's limit: it ignores everything past 72 bytes.
const maxPasswordBytes = 72

// -- C:Y:B . CoA --

// dummyHash is compared against when a username is unknown, so that unknown
// users and wrong passwords cost the same bcrypt time. It is computed on first
// use so importers do not pay for it at init. Generating it can only fail on
// a constant we control, so a failure panics rather than silently turning the
// unknown-user path back into a fast one.
var dummyHash = sync.OnceValue(func() []byte {
	h, err := bcrypt.GenerateFromPassword([]byte("timing-equaliser"), bcrypt.DefaultCost)
	if err != nil {
		panic(err)
	}
	return h
})

// ValidateCredentials checks the shape of a username and password before
// anything touches the database. Its errors wrap ErrInvalidInput and read
// well enough to show to the user.
func ValidateCredentials(username, password string) error {
	if !usernamePattern.MatchString(username) {
		return fmt.Errorf("%w: username must be 3-32 letters, digits, '.', '_' or '-'", ErrInvalidInput)
	}
	if len(password) < 4 || len(password) > maxPasswordBytes {
		return fmt.Errorf("%w: password must be 4-%d bytes", ErrInvalidInput, maxPasswordBytes)
	}
	return nil
}

type Store struct {
	pool *pgxpool.Pool
	ttl  time.Duration
	now  func() time.Time
}

func NewStore(pool *pgxpool.Pool, tokenTTL time.Duration) *Store {
	return &Store{pool: pool, ttl: tokenTTL, now: time.Now}
}

func (s *Store) TokenTTL() time.Duration { return s.ttl }

func (s *Store) Register(ctx context.Context, username, password string) (Account, error) {
	if err := ValidateCredentials(username, password); err != nil {
		return Account{}, err
	}
	hash, err := bcrypt.GenerateFromPassword([]byte(password), bcrypt.DefaultCost)
	if err != nil {
		return Account{}, err
	}
	var id int64
	err = s.pool.QueryRow(ctx,
		"INSERT INTO accounts (username, password_hash) VALUES ($1, $2) RETURNING id",
		username, string(hash)).Scan(&id)
	var pgErr *pgconn.PgError
	if errors.As(err, &pgErr) && pgErr.Code == "23505" && pgErr.ConstraintName == "accounts_username_lower" {
		return Account{}, ErrUsernameTaken
	}
	if err != nil {
		return Account{}, err
	}
	return Account{ID: id, Username: username}, nil
}

// Authenticate checks a username and password. Input that cannot belong to any
// account is rejected before the database is queried; an unknown username
// still pays one bcrypt comparison so it is not distinguishable by timing.
func (s *Store) Authenticate(ctx context.Context, username, password string) (Account, error) {
	if !usernamePattern.MatchString(username) || len(password) > maxPasswordBytes {
		return Account{}, ErrInvalidCredentials
	}
	var acc Account
	var hash string
	err := s.pool.QueryRow(ctx,
		"SELECT id, username, password_hash FROM accounts WHERE lower(username) = lower($1)",
		username).Scan(&acc.ID, &acc.Username, &hash)
	if errors.Is(err, pgx.ErrNoRows) {
		_ = bcrypt.CompareHashAndPassword(dummyHash(), []byte(password))
		return Account{}, ErrInvalidCredentials
	}
	if err != nil {
		return Account{}, err
	}
	if err := bcrypt.CompareHashAndPassword([]byte(hash), []byte(password)); err != nil {
		if errors.Is(err, bcrypt.ErrMismatchedHashAndPassword) {
			return Account{}, ErrInvalidCredentials
		}
		return Account{}, fmt.Errorf("account %d: password hash: %w", acc.ID, err)
	}
	return acc, nil
}

// FindByUsername resolves an account without a password (admin tools).
func (s *Store) FindByUsername(ctx context.Context, username string) (Account, error) {
	if !usernamePattern.MatchString(username) {
		return Account{}, ErrAccountNotFound
	}
	var acc Account
	err := s.pool.QueryRow(ctx,
		"SELECT id, username FROM accounts WHERE lower(username) = lower($1)",
		username).Scan(&acc.ID, &acc.Username)
	if errors.Is(err, pgx.ErrNoRows) {
		return Account{}, ErrAccountNotFound
	}
	return acc, err
}

// IssueToken creates a login token "aoc_" + 64 hex digits; only its SHA-256
// is stored.
func (s *Store) IssueToken(ctx context.Context, accountID int64) (Token, error) {
	var raw [32]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return Token{}, err
	}
	tok := Token{Value: "aoc_" + hex.EncodeToString(raw[:]), ExpiresAt: s.now().Add(s.ttl)}
	sum := sha256.Sum256([]byte(tok.Value))
	if _, err := s.pool.Exec(ctx,
		"INSERT INTO login_tokens (token_hash, account_id, expires_at) VALUES ($1, $2, $3)",
		sum[:], accountID, tok.ExpiresAt); err != nil {
		return Token{}, err
	}
	return tok, nil
}

func (s *Store) ValidateToken(ctx context.Context, token string) (Account, error) {
	sum := sha256.Sum256([]byte(token))
	var acc Account
	err := s.pool.QueryRow(ctx, `
		SELECT a.id, a.username FROM login_tokens t JOIN accounts a ON a.id = t.account_id
		WHERE t.token_hash = $1 AND t.expires_at > $2`, sum[:], s.now()).Scan(&acc.ID, &acc.Username)
	if errors.Is(err, pgx.ErrNoRows) {
		return Account{}, ErrInvalidToken
	}
	return acc, err
}

// PurgeExpiredTokens deletes login tokens past their expiry (the ones
// ValidateToken already rejects) and returns how many it removed.
func (s *Store) PurgeExpiredTokens(ctx context.Context) (int64, error) {
	tag, err := s.pool.Exec(ctx, "DELETE FROM login_tokens WHERE expires_at <= $1", s.now())
	return tag.RowsAffected(), err
}
