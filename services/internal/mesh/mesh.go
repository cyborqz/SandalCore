// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

// Package mesh keeps the registry of world servers (worldd) and decides where
// a character plays. Milestone A runs one world server: placement picks the
// live registered one, or the configured static world (the legacy aoc_server,
// which never registers) while none is live.
package mesh

import (
	"context"
	"errors"
	"fmt"
	"math"
	"net"
	"regexp"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

var (
	ErrInvalid       = errors.New("invalid world server registration")
	ErrUnknownServer = errors.New("world server not registered")
	ErrNoServer      = errors.New("no live world server")
)

// StaticServerID is the ServerId placement reports for the static world.
// Registered servers get ids from 1 up.
const StaticServerID uint32 = 0

type Store struct {
	pool   *pgxpool.Pool
	ttl    time.Duration // a server silent this long is not placed
	static string        // address of the static world, "" for none
	now    func() time.Time
}

func NewStore(pool *pgxpool.Pool, heartbeatTTL time.Duration, staticWorld string) *Store {
	return &Store{pool: pool, ttl: heartbeatTTL, static: staticWorld, now: time.Now}
}

// HeartbeatTTL is how long a server may stay silent and still be placed.
func (s *Store) HeartbeatTTL() time.Duration { return s.ttl }

// Register records a world server at address (host:port) owning regions and
// returns its new ServerId. Registering counts as a first heartbeat.
func (s *Store) Register(ctx context.Context, address string, regions []string) (uint32, error) {
	if err := ValidAddress(address); err != nil {
		return 0, fmt.Errorf("%w: address %q: %v", ErrInvalid, address, err)
	}
	for _, r := range regions {
		if !plainText(r) {
			return 0, fmt.Errorf("%w: region %q", ErrInvalid, r)
		}
	}
	if regions == nil {
		regions = []string{}
	}
	var id int32
	err := s.pool.QueryRow(ctx,
		"INSERT INTO world_servers (address, regions, last_heartbeat) VALUES ($1, $2, $3) RETURNING server_id",
		address, regions, s.now()).Scan(&id)
	return uint32(id), err
}

// Heartbeat marks a registered server as alive. ErrUnknownServer when there is
// no such registration: the server should register again.
func (s *Store) Heartbeat(ctx context.Context, id uint32) error {
	if id == StaticServerID || id > math.MaxInt32 {
		return ErrUnknownServer // no registration can have this id
	}
	tag, err := s.pool.Exec(ctx, "UPDATE world_servers SET last_heartbeat = $2 WHERE server_id = $1", int32(id), s.now())
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return ErrUnknownServer
	}
	return nil
}

// built by CoA with C_Y_B

// Place returns the world server a character should join: the registered
// server with the most recent heartbeat inside the TTL, else the static
// world, else ErrNoServer. With one world server the character does not
// matter yet; the region of its saved position will once there are several.
func (s *Store) Place(ctx context.Context, characterID string) (uint32, string, error) {
	var id int32
	var address string
	err := s.pool.QueryRow(ctx, `
		SELECT server_id, address FROM world_servers
		WHERE last_heartbeat > $1
		ORDER BY last_heartbeat DESC, server_id DESC
		LIMIT 1`, s.now().Add(-s.ttl)).Scan(&id, &address)
	switch {
	case err == nil:
		return uint32(id), address, nil
	case !errors.Is(err, pgx.ErrNoRows):
		return 0, "", err
	case s.static != "":
		return StaticServerID, s.static, nil
	}
	return 0, "", ErrNoServer
}

// ValidAddress checks that address is a host:port a game client can dial: an IP
// address (not 0.0.0.0 or ::, which a server binds) or a plain ASCII host name,
// and a numeric port in 1-65535.
func ValidAddress(address string) error {
	host, port, err := net.SplitHostPort(address)
	if err != nil {
		return err
	}
	if ip := net.ParseIP(host); ip != nil {
		if ip.IsUnspecified() {
			return errors.New("0.0.0.0 and :: are bind addresses, not addresses a client can dial")
		}
	} else if !hostName.MatchString(host) {
		return errors.New("the host is neither an IP address nor a plain ASCII host name")
	}
	if n, err := strconv.ParseUint(port, 10, 16); err != nil || n == 0 {
		return errors.New("port is not a number from 1 to 65535")
	}
	return nil
}

// hostName is what a host name may consist of (letters, digits, dot, hyphen and
// underscore, the last for container names); anything else, Unicode spaces and
// zero-width or bidi format characters included, is a typo the client cannot
// resolve.
var hostName = regexp.MustCompile(`^[A-Za-z0-9._-]+$`)

// plainText reports whether s is UTF-8 without NUL, which PostgreSQL text
// requires.
func plainText(s string) bool {
	return utf8.ValidString(s) && !strings.ContainsRune(s, 0)
}
