// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package tickets issues and redeems one-time join tickets. The gateway issues
// one when a player presses Play; the client presents it to the world server
// in its first UDP handshake packet; the world server redeems it (through the
// gateway's RedeemTicket) before letting the player in.
package tickets

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"math"
	"regexp"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

// ErrInvalid is every reason a ticket cannot be redeemed (unknown, already
// used, expired, or issued for another character or world server; the reasons
// are deliberately not told apart), and a request for a ticket to play a
// character that is not the account's.
var ErrInvalid = errors.New("join ticket not valid")

// hex32 is the shape of tickets and character ids: 32 lowercase hex digits.
var hex32 = regexp.MustCompile(`^[0-9a-f]{32}$`)

type Store struct {
	pool *pgxpool.Pool
	ttl  time.Duration
	now  func() time.Time
}

// built with CoA and C.Y.B

// NewStore issues tickets that stay redeemable for ttl.
func NewStore(pool *pgxpool.Pool, ttl time.Duration) *Store {
	return &Store{pool: pool, ttl: ttl, now: time.Now}
}

// Issue creates a ticket for accountID to join serverID as characterID. The
// ticket is 128 random bits as 32 lowercase hex digits, which the client
// carries as a GUID. characterID must be the account's character (ErrInvalid
// otherwise); the check and the insert are one statement, so a character
// deleted meanwhile gets no ticket.
func (s *Store) Issue(ctx context.Context, accountID int64, characterID string, serverID uint32) (string, error) {
	if !hex32.MatchString(characterID) || serverID > math.MaxInt32 {
		return "", ErrInvalid
	}
	var raw [16]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", err
	}
	ticket := hex.EncodeToString(raw[:])
	now := s.now()
	tag, err := s.pool.Exec(ctx, `
		INSERT INTO sessions (ticket, account_id, character_id, target_server_id, created_at, expires_at)
		SELECT $1, account_id, id, $4, $5, $6 FROM characters WHERE id = $3 AND account_id = $2`,
		ticket, accountID, characterID, int32(serverID), now, now.Add(s.ttl))
	var pgErr *pgconn.PgError
	if errors.As(err, &pgErr) && pgErr.Code == "23503" {
		return "", ErrInvalid // the character was deleted while the ticket was written
	}
	if err != nil {
		return "", err
	}
	if tag.RowsAffected() == 0 {
		return "", ErrInvalid
	}
	return ticket, nil
}

// Redeem spends the ticket if it was issued for characterID on serverID, is
// unused and has not expired, and returns the account it was issued to. A
// failed attempt does not spend it. serverID 0 is the static world (the
// legacy server, which does not redeem tickets) and is also what an unset
// proto3 field looks like, so it never redeems, whatever was issued.
func (s *Store) Redeem(ctx context.Context, ticket, characterID string, serverID uint32) (int64, error) {
	if serverID == 0 {
		return 0, ErrInvalid
	}
	if !hex32.MatchString(ticket) || !hex32.MatchString(characterID) || serverID > math.MaxInt32 {
		return 0, ErrInvalid // nothing issued looks like this; keep NUL and bad UTF-8 away from PostgreSQL
	}
	now := s.now()
	var accountID int64
	err := s.pool.QueryRow(ctx, `
		UPDATE sessions SET used_at = $4
		WHERE ticket = $1 AND character_id = $2 AND target_server_id = $3
		  AND used_at IS NULL AND expires_at > $4
		RETURNING account_id`, ticket, characterID, int32(serverID), now).Scan(&accountID)
	if errors.Is(err, pgx.ErrNoRows) {
		return 0, ErrInvalid
	}
	return accountID, err
}

// Purge deletes the tickets past their expiry, spent or not, and returns how
// many it removed.
func (s *Store) Purge(ctx context.Context) (int64, error) {
	tag, err := s.pool.Exec(ctx, "DELETE FROM sessions WHERE expires_at <= $1", s.now())
	return tag.RowsAffected(), err
}
